#include "PluginEditor.h"

static juce::Font monoFont (float size, bool bold = false)
{
    return juce::Font (juce::Font::getDefaultMonospacedFontName(), size,
                       bold ? juce::Font::bold : juce::Font::plain);
}

//==============================================================================
// BeatPadComponent
//==============================================================================
BeatPadComponent::BeatPadComponent (std::atomic<bool>& activeRef,
                                     std::atomic<float>& velocityRef,
                                     std::atomic<int>&   noteRef,
                                     std::atomic<float>& gateRef,
                                     std::atomic<float>& cutoffRef,
                                     int index)
    : active (activeRef), velocity (velocityRef), note (noteRef),
      gate (gateRef), cutoff (cutoffRef), idx (index)
{
    setRepaintsOnMouseActivity (true);

    // Per-beat gate slider
    gateSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    gateSlider.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
    gateSlider.setRange (0.01, 1.0, 0.01);
    gateSlider.setValue (gate.load(), juce::dontSendNotification);
    gateSlider.setColour (juce::Slider::trackColourId,      Theme::Accent);
    gateSlider.setColour (juce::Slider::backgroundColourId, Theme::Border);
    gateSlider.setColour (juce::Slider::thumbColourId,      Theme::Accent);
    gateSlider.onValueChange = [this] { gate.store ((float) gateSlider.getValue()); };
    addAndMakeVisible (gateSlider);

    // Per-beat LP cutoff slider
    cutoffSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    cutoffSlider.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
    cutoffSlider.setRange (0.0, 1.0, 0.01);
    cutoffSlider.setValue (cutoff.load(), juce::dontSendNotification);
    cutoffSlider.setColour (juce::Slider::trackColourId,      Theme::CutoffBlue);
    cutoffSlider.setColour (juce::Slider::backgroundColourId, Theme::Border);
    cutoffSlider.setColour (juce::Slider::thumbColourId,      Theme::CutoffBlue);
    cutoffSlider.onValueChange = [this] { cutoff.store ((float) cutoffSlider.getValue()); };
    addAndMakeVisible (cutoffSlider);
}

void BeatPadComponent::setFlash (float amount)
{
    flashAmount = amount;
    repaint();
}

void BeatPadComponent::paint (juce::Graphics& g)
{
    const bool  isActive = active.load();
    const float vel      = velocity.load();

    // Background
    g.setColour (hovering ? Theme::BeatBgHover : Theme::BeatBg);
    g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (1.0f), 3.0f);

    // Orange active bar at top (height scales with velocity)
    if (isActive)
    {
        const float barH = 6.0f + vel * (float)getHeight() * 0.15f;
        g.setColour (Theme::Accent);
        g.fillRoundedRectangle (1.0f, 1.0f, (float)getWidth() - 2.0f, barH, 3.0f);
    }

    // White flash pulse overlay — fades out after beat fires
    if (flashAmount > 0.002f)
    {
        g.setColour (Theme::Flash.withAlpha (flashAmount * 0.35f));
        g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (1.0f), 3.0f);

        // Bright top edge
        g.setColour (Theme::Flash.withAlpha (flashAmount * 0.9f));
        g.fillRoundedRectangle (1.0f, 1.0f, (float)getWidth() - 2.0f, 3.0f, 2.0f);
    }

    // Note name — upper portion of pad
    const juce::String noteName = juce::MidiMessage::getMidiNoteName (note.load(), true, true, 4);
    const float noteFontSize = juce::jlimit (9.0f, 15.0f, (float)getWidth() * 0.38f);
    g.setFont (monoFont (noteFontSize, true));
    g.setColour (isActive ? Theme::TextAccent : Theme::TextDim);
    const int noteY = (int)(getHeight() * 0.22f);
    g.drawText (noteName, 2, noteY, getWidth() - 4, 20,
                juce::Justification::centred, false);

    // Slider labels
    g.setFont (monoFont (7.0f));
    g.setColour (Theme::TextDim.withAlpha (0.5f));
    const int gateTopY = (int)(getHeight() * 0.42f);
    g.drawText ("GATE", 0, gateTopY, getWidth(), 10, juce::Justification::centred, false);
    const int cutoffTopY = (int)(getHeight() * 0.62f);
    g.drawText ("LPF", 0, cutoffTopY, getWidth(), 10, juce::Justification::centred, false);

    // Beat index label
    g.setFont (monoFont (9.0f));
    g.setColour (Theme::TextDim.withAlpha (0.6f));
    g.drawText (juce::String::formatted ("%02d", idx + 1),
                0, getHeight() - 14, getWidth(), 12,
                juce::Justification::centred, false);
}

void BeatPadComponent::mouseDown (const juce::MouseEvent&)
{
    active.store (!active.load());
    if (onChange) onChange();
    repaint();
}

void BeatPadComponent::mouseEnter (const juce::MouseEvent&) { hovering = true;  repaint(); }
void BeatPadComponent::mouseExit  (const juce::MouseEvent&) { hovering = false; repaint(); }

void BeatPadComponent::mouseWheelMove (const juce::MouseEvent&,
                                        const juce::MouseWheelDetails& wheel)
{
    const int delta = wheel.deltaY > 0 ? 1 : (wheel.deltaY < 0 ? -1 : 0);
    if (delta == 0) return;
    const double now = juce::Time::getMillisecondCounterHiRes();
    if (now - lastWheelMs < 100.0) return;
    lastWheelMs = now;
    changeNote (delta);
}

void BeatPadComponent::changeNote (int delta)
{
    note.store (juce::jlimit (0, 127, note.load() + delta));
    if (onChange) onChange();
    repaint();
}

void BeatPadComponent::resized()
{
    const int w = getWidth();
    const int h = getHeight();
    const int padX = 4;
    const int sliderW = w - 2 * padX;
    const int sliderH = 14;

    const int gateY = (int)(h * 0.52f);
    gateSlider.setBounds (padX, gateY, sliderW, sliderH);

    const int cutoffY = (int)(h * 0.72f);
    cutoffSlider.setBounds (padX, cutoffY, sliderW, sliderH);
}

void BeatPadComponent::updateGateSlider()
{
    gateSlider.setValue (gate.load(), juce::dontSendNotification);
}

void BeatPadComponent::updateCutoffSlider()
{
    cutoffSlider.setValue (cutoff.load(), juce::dontSendNotification);
}

//==============================================================================
// ScrollableNoteLabel
//==============================================================================
ScrollableNoteLabel::ScrollableNoteLabel()
{
    setRepaintsOnMouseActivity (true);
}

void ScrollableNoteLabel::setNoteName (const juce::String& name)
{
    currentNote = name;
    repaint();
}

void ScrollableNoteLabel::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();

    // Subtle background
    g.setColour (hovering ? Theme::BeatBgHover : Theme::BeatBg);
    g.fillRoundedRectangle (bounds, 3.0f);

    // Border
    g.setColour (hovering ? Theme::Accent : Theme::Border);
    g.drawRoundedRectangle (bounds.reduced (0.5f), 3.0f, 1.0f);

    // Note name
    g.setFont (monoFont (13.0f, true));
    g.setColour (Theme::TextAccent);
    g.drawText (currentNote, bounds, juce::Justification::centred, false);

    // Scroll hint arrows
    g.setFont (monoFont (8.0f));
    g.setColour (Theme::TextDim.withAlpha (0.5f));
    g.drawText (juce::CharPointer_UTF8 ("\xe2\x96\xb2"), bounds.removeFromTop (10),
                juce::Justification::centredTop, false);
    g.drawText (juce::CharPointer_UTF8 ("\xe2\x96\xbc"), getLocalBounds().toFloat().removeFromBottom (10),
                juce::Justification::centredBottom, false);
}

void ScrollableNoteLabel::mouseWheelMove (const juce::MouseEvent&,
                                           const juce::MouseWheelDetails& wheel)
{
    const int delta = wheel.deltaY > 0 ? 1 : (wheel.deltaY < 0 ? -1 : 0);
    if (delta == 0 || onNoteChange == nullptr) return;
    const double now = juce::Time::getMillisecondCounterHiRes();
    if (now - lastWheelMs < 100.0) return;
    lastWheelMs = now;
    onNoteChange (delta);
}

void ScrollableNoteLabel::mouseDown (const juce::MouseEvent& e)
{
    // Click top half = up, bottom half = down
    const int delta = e.y < getHeight() / 2 ? 1 : -1;
    if (onNoteChange) onNoteChange (delta);
}

//==============================================================================
// ScrollableSoundLabel
//==============================================================================
ScrollableSoundLabel::ScrollableSoundLabel()
{
    setRepaintsOnMouseActivity (true);
}

void ScrollableSoundLabel::setSoundName (const juce::String& name)
{
    currentSound = name;
    repaint();
}

void ScrollableSoundLabel::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();

    g.setColour (hovering ? Theme::BeatBgHover : Theme::BeatBg);
    g.fillRoundedRectangle (bounds, 3.0f);

    g.setColour (hovering ? Theme::Accent : Theme::Border);
    g.drawRoundedRectangle (bounds.reduced (0.5f), 3.0f, 1.0f);

    g.setFont (monoFont (11.0f, true));
    g.setColour (Theme::TextPrimary);
    g.drawText (currentSound, bounds, juce::Justification::centred, false);

    // Scroll hint arrows
    g.setFont (monoFont (8.0f));
    g.setColour (Theme::TextDim.withAlpha (0.5f));
    g.drawText (juce::CharPointer_UTF8 ("\xe2\x96\xb2"), bounds.removeFromTop (10),
                juce::Justification::centredTop, false);
    g.drawText (juce::CharPointer_UTF8 ("\xe2\x96\xbc"), getLocalBounds().toFloat().removeFromBottom (10),
                juce::Justification::centredBottom, false);
}

void ScrollableSoundLabel::mouseWheelMove (const juce::MouseEvent&,
                                            const juce::MouseWheelDetails& wheel)
{
    const int delta = wheel.deltaY > 0 ? 1 : (wheel.deltaY < 0 ? -1 : 0);
    if (delta != 0 && onSoundChange) onSoundChange (delta);
}

void ScrollableSoundLabel::mouseDown (const juce::MouseEvent& e)
{
    const int delta = e.y < getHeight() / 2 ? 1 : -1;
    if (onSoundChange) onSoundChange (delta);
}

//==============================================================================
// RhythmTrackComponent
//==============================================================================
RhythmTrackComponent::RhythmTrackComponent (PolyrhythmProcessor& p, bool isA)
    : processor (p), trackA (isA)
{
    auto makeLabel = [this] (juce::Label& lbl, const juce::String& txt,
                              float size, juce::Colour col, bool bold = false)
    {
        lbl.setText (txt, juce::dontSendNotification);
        lbl.setFont (monoFont (size, bold));
        lbl.setColour (juce::Label::textColourId, col);
        lbl.setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (lbl);
    };

    makeLabel (trackNameLabel, isA ? "RHYTHM_A" : "RHYTHM_B", 14.0f, Theme::TextPrimary, true);
    makeLabel (beatsLabel,     "BEATS",                        10.0f, Theme::TextDim);
    makeLabel (beatCountLabel, "4",                            14.0f, Theme::TextPrimary, true);
    makeLabel (noteLabel,      "NOTE",                         10.0f, Theme::TextDim);
    makeLabel (soundLabel,     "SOUND",                        10.0f, Theme::TextDim);
    makeLabel (gateLabel,      "GATE",                         10.0f, Theme::TextDim);

    // Channel badge
    channelLabel.setText (isA ? "CH 01" : "CH 02", juce::dontSendNotification);
    channelLabel.setFont (monoFont (10.0f));
    channelLabel.setColour (juce::Label::textColourId,       Theme::TextPrimary);
    channelLabel.setColour (juce::Label::backgroundColourId, Theme::Accent);
    channelLabel.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (channelLabel);

    auto styleBtn = [this] (juce::TextButton& btn)
    {
        btn.setColour (juce::TextButton::buttonColourId,  Theme::BeatBg);
        btn.setColour (juce::TextButton::textColourOnId,  Theme::TextPrimary);
        btn.setColour (juce::TextButton::textColourOffId, Theme::TextPrimary);
        addAndMakeVisible (btn);
    };

    styleBtn (decBtn);    styleBtn (incBtn);
    decBtn.onClick    = [this] { changeBeatCount (-1); };
    incBtn.onClick    = [this] { changeBeatCount (+1); };

    // Scrollable note selector
    noteScroller.onNoteChange = [this] (int delta) { changeNote (delta); };
    addAndMakeVisible (noteScroller);

    // Reset notes button
    resetNotesBtn.setButtonText (juce::String (juce::CharPointer_UTF8 ("\xe2\x86\xba")));
    resetNotesBtn.setColour (juce::TextButton::buttonColourId,  Theme::BeatBg);
    resetNotesBtn.setColour (juce::TextButton::textColourOnId,  Theme::TextDim);
    resetNotesBtn.setColour (juce::TextButton::textColourOffId, Theme::TextDim);
    resetNotesBtn.onClick = [this] { resetNotes(); };
    addAndMakeVisible (resetNotesBtn);

    // Scrollable sound type selector
    soundScroller.onSoundChange = [this] (int delta) { changeSoundType (delta); };
    addAndMakeVisible (soundScroller);

    // Gate slider (track-level: sets all per-beat gates when dragged)
    gateSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    gateSlider.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
    gateSlider.setRange (0.01, 1.0, 0.01);
    gateSlider.setColour (juce::Slider::trackColourId,      Theme::Accent);
    gateSlider.setColour (juce::Slider::backgroundColourId, Theme::Border);
    gateSlider.setColour (juce::Slider::thumbColourId,      Theme::Accent);
    addAndMakeVisible (gateSlider);

    gateAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>
                 (processor.apvts, isA ? "trackA_gate" : "trackB_gate", gateSlider);

    gateSlider.onValueChange = [this] {
        if (!juce::Component::isMouseButtonDownAnywhere()) return;
        float g = (float) gateSlider.getValue();
        auto& gateArr = trackA ? processor.trackAGate : processor.trackBGate;
        for (int i = 0; i < MAX_BEATS; ++i)
        {
            gateArr[i].store (g);
            if (pads[i]) pads[i]->updateGateSlider();
        }
    };

    // Beat pads
    auto& activeArr  = trackA ? processor.trackAActive   : processor.trackBActive;
    auto& velArr     = trackA ? processor.trackAVelocity : processor.trackBVelocity;
    auto& noteArr    = trackA ? processor.trackANotes    : processor.trackBNotes;
    auto& gateArr    = trackA ? processor.trackAGate     : processor.trackBGate;
    auto& cutoffArr  = trackA ? processor.trackACutoff   : processor.trackBCutoff;

    for (int i = 0; i < MAX_BEATS; ++i)
    {
        pads[i] = std::make_unique<BeatPadComponent> (activeArr[i], velArr[i], noteArr[i],
                                                       gateArr[i], cutoffArr[i], i);
        pads[i]->onChange = [this] { repaint(); };
        addAndMakeVisible (*pads[i]);
    }

    updateBeatCountLabel();
    updateNoteLabel();
    updateSoundLabel();
    refreshPads();

    // Initialise fire-count baseline so we don't flash on first load
    lastFireCount = trackA ? processor.trackAFireCount.load()
                           : processor.trackBFireCount.load();

    startTimerHz (30);   // 30 Hz animation timer
}

void RhythmTrackComponent::timerCallback()
{
    // Check if a new beat has fired since last tick
    const uint64_t current = trackA ? processor.trackAFireCount.load()
                                    : processor.trackBFireCount.load();
    if (current != lastFireCount)
    {
        lastFireCount = current;
        flashBeatIdx  = trackA ? processor.trackACurrentBeat.load()
                               : processor.trackBCurrentBeat.load();
        flashAmount   = 1.0f;
        updateNoteLabel();   // keep "ALL" note display in sync
    }

    // Decay and repaint the flashing pad
    if (flashAmount > 0.002f)
    {
        flashAmount *= 0.62f;   // ~5 frames to near-zero at 30 Hz ≈ 165 ms decay

        const int count = trackA ? processor.getTrackABeatCount()
                                 : processor.getTrackBBeatCount();
        if (flashBeatIdx >= 0 && flashBeatIdx < count && pads[flashBeatIdx])
            pads[flashBeatIdx]->setFlash (flashAmount);
    }
    else if (flashAmount > 0.0f)
    {
        // Clear the pad flash when fully decayed
        if (flashBeatIdx >= 0 && pads[flashBeatIdx])
            pads[flashBeatIdx]->setFlash (0.0f);
        flashAmount  = 0.0f;
        flashBeatIdx = -1;
    }
}

void RhythmTrackComponent::changeBeatCount (int delta)
{
    auto* param = trackA
        ? dynamic_cast<juce::AudioParameterInt*> (processor.apvts.getParameter ("trackA_beats"))
        : dynamic_cast<juce::AudioParameterInt*> (processor.apvts.getParameter ("trackB_beats"));

    if (param)
        *param = juce::jlimit (1, 16, param->get() + delta);

    updateBeatCountLabel();
    refreshPads();
    resized();
}

void RhythmTrackComponent::changeNote (int delta)
{
    if (trackA)
        processor.shiftTrackANotes (delta);
    else
        processor.shiftTrackBNotes (delta);

    updateNoteLabel();
    repaint();   // pads update their displayed note names
}

void RhythmTrackComponent::updateBeatCountLabel()
{
    const int count = trackA ? processor.getTrackABeatCount()
                             : processor.getTrackBBeatCount();
    beatCountLabel.setText (juce::String (count), juce::dontSendNotification);
}

void RhythmTrackComponent::updateNoteLabel()
{
    const int note = trackA ? processor.trackANotes[0].load()
                            : processor.trackBNotes[0].load();
    noteScroller.setNoteName (juce::MidiMessage::getMidiNoteName (note, true, true, 4));
}

void RhythmTrackComponent::changeSoundType (int delta)
{
    auto& st = trackA ? processor.trackASoundType : processor.trackBSoundType;
    int cur = st.load();
    cur = (cur + delta + NUM_SOUND_TYPES) % NUM_SOUND_TYPES;
    st.store (cur);
    updateSoundLabel();
}

void RhythmTrackComponent::updateSoundLabel()
{
    const int st = trackA ? processor.trackASoundType.load()
                          : processor.trackBSoundType.load();
    soundScroller.setSoundName (soundTypeName ((SoundType)st));
}

void RhythmTrackComponent::resetNotes()
{
    if (trackA)
        processor.resetTrackANotes();
    else
        processor.resetTrackBNotes();
    updateNoteLabel();
    repaint();
}

void RhythmTrackComponent::refreshPads()
{
    const int count = trackA ? processor.getTrackABeatCount()
                             : processor.getTrackBBeatCount();
    for (int i = 0; i < MAX_BEATS; ++i)
        pads[i]->setVisible (i < count);
}

void RhythmTrackComponent::paint (juce::Graphics& g)
{
    g.setColour (Theme::Panel);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 4.0f);

    // Accent left border
    g.setColour (Theme::Accent);
    g.fillRect (0, 0, 3, getHeight());

    // Sub-label
    g.setFont (monoFont (9.0f));
    g.setColour (Theme::TextDim);
    g.drawText (trackA ? "POLY_LOGIC_01" : "POLY_LOGIC_02",
                12, 28, 200, 14, juce::Justification::centredLeft, false);
}

void RhythmTrackComponent::resized()
{
    const int w = getWidth();
    const int H = getHeight();
    const int btnH = 24;

    // Header
    trackNameLabel.setBounds (12, 8,     180, 20);
    channelLabel.setBounds   (w - 54, 8, 46,  18);

    // Beats row
    const int beatsY = 46;
    beatsLabel.setBounds     (12,      beatsY + 4, 50, 16);
    decBtn.setBounds         (w - 86,  beatsY, btnH, btnH);
    beatCountLabel.setBounds (w - 60,  beatsY + 2, 24, 20);
    incBtn.setBounds         (w - 34,  beatsY, btnH, btnH);

    // Note row (scrollable) + reset button
    const int noteY = beatsY + btnH + 8;
    const int scrollH = 36;
    noteLabel.setBounds       (12,  noteY + 8, 40, 16);
    noteScroller.setBounds    (58,  noteY,     60, scrollH);
    resetNotesBtn.setBounds   (122, noteY + 6, btnH, btnH);

    // Sound type row (scrollable)
    soundLabel.setBounds      (152, noteY + 8, 50, 16);
    soundScroller.setBounds   (202, noteY,     60, scrollH);

    // Gate row
    const int gateY = noteY + scrollH + 8;
    gateLabel.setBounds  (12, gateY + 4, 40,       16);
    gateSlider.setBounds (56, gateY,     w - 68,   btnH);

    // Beat pads — fill remaining height
    const int padAreaY = gateY + btnH + 12;
    const int padAreaH = H - padAreaY - 10;
    const int count    = trackA ? processor.getTrackABeatCount()
                                : processor.getTrackBBeatCount();
    const int gap      = 5;
    const int padW     = (w - 16 - (count - 1) * gap) / std::max (1, count);

    for (int i = 0; i < MAX_BEATS; ++i)
        if (i < count)
            pads[i]->setBounds (8 + i * (padW + gap), padAreaY, padW, padAreaH);
}

//==============================================================================
// LabelledSlider
//==============================================================================
LabelledSlider::LabelledSlider (const juce::String& labelText, juce::Colour fillColour)
    : colour (fillColour)
{
    label.setText (labelText, juce::dontSendNotification);
    label.setFont (monoFont (9.0f));
    label.setColour (juce::Label::textColourId, Theme::TextDim);
    label.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (label);

    slider.setSliderStyle (juce::Slider::LinearHorizontal);
    slider.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
    slider.setColour (juce::Slider::trackColourId,      fillColour);
    slider.setColour (juce::Slider::backgroundColourId, Theme::Border);
    slider.setColour (juce::Slider::thumbColourId,      fillColour);
    addAndMakeVisible (slider);
}

void LabelledSlider::resized()
{
    const int lw = 46;
    label.setBounds  (0,       0, lw,              getHeight());
    slider.setBounds (lw + 4,  0, getWidth() - lw - 4, getHeight());
}

//==============================================================================
// PolyrhythmEditor
//==============================================================================
PolyrhythmEditor::PolyrhythmEditor (PolyrhythmProcessor& p)
    : AudioProcessorEditor (&p), audioProcessor (p),
      trackAComp (p, true), trackBComp (p, false)
{
    setSize (1024, 580);

    // Title
    titleLabel.setText ("POLYRHYTHM_MODULE_V1", juce::dontSendNotification);
    titleLabel.setFont (monoFont (13.0f, true));
    titleLabel.setColour (juce::Label::textColourId, Theme::Accent);
    addAndMakeVisible (titleLabel);

    // BPM display
    bpmValueLabel.setText ("---", juce::dontSendNotification);
    bpmValueLabel.setFont (monoFont (28.0f, true));
    bpmValueLabel.setColour (juce::Label::textColourId, Theme::Accent);
    bpmValueLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (bpmValueLabel);

    bpmUnitLabel.setText ("BPM", juce::dontSendNotification);
    bpmUnitLabel.setFont (monoFont (10.0f));
    bpmUnitLabel.setColour (juce::Label::textColourId, Theme::TextDim);
    addAndMakeVisible (bpmUnitLabel);

    // Global sliders
    swingSlider.slider.setRange (0.0, 0.5, 0.01);
    probSlider.slider.setRange  (0.0, 1.0, 0.01);

    swingAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>
                  (audioProcessor.apvts, "swing",       swingSlider.slider);
    probAttach  = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>
                  (audioProcessor.apvts, "probability", probSlider.slider);

    addAndMakeVisible (swingSlider);
    addAndMakeVisible (probSlider);

    // Audio preview toggle
    audioToggleBtn.setColour (juce::TextButton::buttonColourId,   Theme::BeatBg);
    audioToggleBtn.setColour (juce::TextButton::buttonOnColourId, Theme::Accent);
    audioToggleBtn.setColour (juce::TextButton::textColourOnId,   Theme::TextPrimary);
    audioToggleBtn.setColour (juce::TextButton::textColourOffId,  Theme::TextDim);
    audioToggleBtn.setClickingTogglesState (true);
    audioToggleBtn.setToggleState (audioProcessor.audioPreviewEnabled.load(), juce::dontSendNotification);
    audioToggleBtn.onClick = [this] {
        audioProcessor.audioPreviewEnabled.store (audioToggleBtn.getToggleState());
    };
    addAndMakeVisible (audioToggleBtn);

    // Track panels
    addAndMakeVisible (trackAComp);
    addAndMakeVisible (trackBComp);

    startTimerHz (10);   // BPM readout
}

PolyrhythmEditor::~PolyrhythmEditor() { stopTimer(); }

void PolyrhythmEditor::timerCallback()
{
    const double bpm = audioProcessor.currentBpm.load();
    if (bpm > 0.0)
        bpmValueLabel.setText (juce::String ((int)std::round (bpm)),
                               juce::dontSendNotification);
}

void PolyrhythmEditor::paint (juce::Graphics& g)
{
    const int W = getWidth();
    const int H = getHeight();

    g.fillAll (Theme::BG);

    // Header bar
    g.setColour (Theme::PanelDark);
    g.fillRect (0, 0, W, 44);
    g.setColour (Theme::Border);
    g.drawLine (0, 44, W, 44, 1.0f);

    // Vertical divider between tracks
    g.setColour (Theme::Accent);
    g.fillRect (W / 2 - 1, 50, 2, H - 54);
}

void PolyrhythmEditor::resized()
{
    const int W = getWidth();
    const int H = getHeight();

    // Header row
    titleLabel.setBounds     (12,       8,  280, 28);
    audioToggleBtn.setBounds (300,     10,  56,  24);
    bpmValueLabel.setBounds  (W - 190,  4,  80,  36);
    bpmUnitLabel.setBounds (W - 106,  20, 30,  16);
    swingSlider.setBounds  (W - 340,  12, 130, 20);
    probSlider.setBounds   (W - 200,  12, 80,  20);   // slight gap before BPM

    // Wait — probSlider overlaps bpmValueLabel; shift left
    swingSlider.setBounds  (W - 360,  12, 130, 20);
    probSlider.setBounds   (W - 220,  12, 100, 20);

    // Track panels — fill everything below the header
    const int trackY = 50;
    const int trackH = H - trackY - 4;
    const int halfW  = W / 2 - 2;
    trackAComp.setBounds (2,          trackY, halfW, trackH);
    trackBComp.setBounds (halfW + 4,  trackY, halfW, trackH);
}
