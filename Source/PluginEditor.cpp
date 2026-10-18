#include "PluginEditor.h"

static juce::Font monoFont (float size, bool bold = false)
{
    return juce::Font (juce::Font::getDefaultMonospacedFontName(), size,
                       bold ? juce::Font::bold : juce::Font::plain);
}

//==============================================================================
// NudgeButton
//==============================================================================
NudgeButton::NudgeButton (bool isPlus)
    : juce::Button (isPlus ? "+" : "-"), plus (isPlus)
{
    setRepaintsOnMouseActivity (true);
}

void NudgeButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    auto bounds = getLocalBounds().toFloat().reduced (0.5f);

    g.setColour (down ? Theme::PanelDark : (highlighted ? Theme::BeatBgHover : Theme::PanelDark.brighter (0.05f)));
    g.fillRoundedRectangle (bounds, 3.0f);

    g.setColour (highlighted ? Theme::Accent : Theme::Border);
    g.drawRoundedRectangle (bounds, 3.0f, 1.0f);

    // Glyph: a square stroke box a third of the button, centred
    const float size   = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.36f;
    const float stroke = juce::jmax (1.2f, size * 0.2f);
    const auto  c      = bounds.getCentre();

    g.setColour (highlighted ? Theme::Accent : Theme::TextPrimary.withAlpha (0.85f));
    g.fillRoundedRectangle (c.x - size / 2.0f, c.y - stroke / 2.0f, size, stroke, stroke / 2.0f);
    if (plus)
        g.fillRoundedRectangle (c.x - stroke / 2.0f, c.y - size / 2.0f, stroke, size, stroke / 2.0f);
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

    // Per-beat velocity slider
    velocitySlider.setSliderStyle (juce::Slider::LinearHorizontal);
    velocitySlider.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
    velocitySlider.setRange (0.0, 1.0, 0.01);
    velocitySlider.setValue (velocity.load(), juce::dontSendNotification);
    velocitySlider.setColour (juce::Slider::trackColourId,      Theme::TextPrimary.withAlpha (0.7f));
    velocitySlider.setColour (juce::Slider::backgroundColourId, Theme::Border);
    velocitySlider.setColour (juce::Slider::thumbColourId,      Theme::TextPrimary);
    velocitySlider.onValueChange = [this]
    {
        velocity.store ((float) velocitySlider.getValue());
        if (onChange) onChange();
    };
    addAndMakeVisible (velocitySlider);

    // Per-beat gate slider
    gateSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    gateSlider.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
    gateSlider.setRange (0.01, 1.0, 0.01);
    gateSlider.setValue (gate.load(), juce::dontSendNotification);
    gateSlider.setColour (juce::Slider::trackColourId,      Theme::Accent);
    gateSlider.setColour (juce::Slider::backgroundColourId, Theme::Border);
    gateSlider.setColour (juce::Slider::thumbColourId,      Theme::Accent);
    gateSlider.onValueChange = [this]
    {
        gate.store ((float) gateSlider.getValue());
        if (onChange) onChange();
    };
    addAndMakeVisible (gateSlider);

    // Per-beat LP cutoff slider
    cutoffSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    cutoffSlider.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
    cutoffSlider.setRange (0.0, 1.0, 0.01);
    cutoffSlider.setValue (cutoff.load(), juce::dontSendNotification);
    cutoffSlider.setColour (juce::Slider::trackColourId,      Theme::CutoffBlue);
    cutoffSlider.setColour (juce::Slider::backgroundColourId, Theme::Border);
    cutoffSlider.setColour (juce::Slider::thumbColourId,      Theme::CutoffBlue);
    cutoffSlider.onValueChange = [this]
    {
        cutoff.store ((float) cutoffSlider.getValue());
        if (onChange) onChange();
    };
    addAndMakeVisible (cutoffSlider);

    // Note nudge buttons — the web version has these on every pad
    addAndMakeVisible (noteDownBtn);
    addAndMakeVisible (noteUpBtn);

    noteDownBtn.onClick = [this] { changeNote (-1); };
    noteUpBtn.onClick   = [this] { changeNote (+1); };
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

    // Orange active bar at top. Scaling this by pad height turned it into a 49px slab on
    // a full-height pad; the web version draws a flat 6px bar, so keep it in that range
    // and let velocity nudge it rather than drive it.
    if (isActive)
    {
        const float barH = juce::jlimit (4.0f, 12.0f, 6.0f + vel * 6.0f);
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

    // Note name. When the nudge buttons are visible they own the sides of the row, so the
    // text is drawn in the gap between them.
    const juce::String noteName = juce::MidiMessage::getMidiNoteName (note.load(), true, true, 4);
    const float noteFontSize = juce::jlimit (8.0f, 16.0f, (float)getWidth() * 0.3f);
    g.setFont (monoFont (noteFontSize, true));
    g.setColour (isActive ? Theme::TextAccent : Theme::TextDim);

    const int noteY = (int)(getHeight() * 0.20f);
    const int noteH = juce::jmax (14, (int)(getHeight() * 0.16f));
    const int inset = showNoteButtons() ? (int)(getWidth() * 0.28f) : 2;
    g.drawText (noteName, inset, noteY, getWidth() - 2 * inset, noteH,
                juce::Justification::centred, false);

    if (showSliders())
    {
        g.setFont (monoFont (juce::jlimit (6.0f, 9.0f, (float)getWidth() * 0.14f)));
        g.setColour (Theme::TextDim.withAlpha (0.5f));
        g.drawText ("VEL",  0, (int)(getHeight() * 0.37f), getWidth(), 10,
                    juce::Justification::centred, false);
        g.drawText ("GATE", 0, (int)(getHeight() * 0.53f), getWidth(), 10,
                    juce::Justification::centred, false);
        g.drawText ("LPF",  0, (int)(getHeight() * 0.69f), getWidth(), 10,
                    juce::Justification::centred, false);
    }

    // Beat index label
    g.setFont (monoFont (juce::jlimit (7.0f, 10.0f, (float)getWidth() * 0.16f)));
    g.setColour (Theme::TextDim.withAlpha (0.6f));
    g.drawText (juce::String::formatted ("%02d", idx + 1),
                0, getHeight() - 14, getWidth(), 12,
                juce::Justification::centred, false);
}

void BeatPadComponent::mouseDown (const juce::MouseEvent&)
{
    active.store (! active.load());
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
    note.store (stepNote ? stepNote (note.load(), delta)
                         : juce::jlimit (0, 127, note.load() + delta));
    if (onChange) onChange();
    repaint();
}

void BeatPadComponent::resized()
{
    const int w = getWidth();
    const int h = getHeight();
    const int padX = juce::jmax (2, w / 12);

    // Note nudge buttons flank the note name; they disappear on narrow pads rather than
    // overlapping the text
    const bool withButtons = showNoteButtons();
    noteDownBtn.setVisible (withButtons);
    noteUpBtn.setVisible   (withButtons);

    if (withButtons)
    {
        const int btnW = juce::jlimit (14, 22, (int)(w * 0.24f));
        const int btnH = juce::jlimit (14, 20, (int)(h * 0.14f));
        const int btnY = (int)(h * 0.20f) + juce::jmax (0, (int)(h * 0.16f) - btnH) / 2;

        noteDownBtn.setBounds (2,            btnY, btnW, btnH);
        noteUpBtn.setBounds   (w - btnW - 2, btnY, btnW, btnH);
    }

    const bool withSliders = showSliders();
    velocitySlider.setVisible (withSliders);
    gateSlider.setVisible     (withSliders);
    cutoffSlider.setVisible   (withSliders);

    if (withSliders)
    {
        const int sliderW = w - 2 * padX;
        const int sliderH = juce::jlimit (10, 16, (int)(h * 0.08f));

        velocitySlider.setBounds (padX, (int)(h * 0.44f), sliderW, sliderH);
        gateSlider.setBounds     (padX, (int)(h * 0.60f), sliderW, sliderH);
        cutoffSlider.setBounds   (padX, (int)(h * 0.76f), sliderW, sliderH);
    }
}

void BeatPadComponent::updateGateSlider()
{
    gateSlider.setValue (gate.load(), juce::dontSendNotification);
}

void BeatPadComponent::updateCutoffSlider()
{
    cutoffSlider.setValue (cutoff.load(), juce::dontSendNotification);
}

void BeatPadComponent::updateVelocitySlider()
{
    velocitySlider.setValue (velocity.load(), juce::dontSendNotification);
}

void BeatPadComponent::refreshNoteDisplay()
{
    repaint();
}

void BeatPadComponent::setUiScale (float newScale)
{
    if (juce::approximatelyEqual (uiScale, newScale)) return;

    uiScale = newScale;
    resized();
    repaint();
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
    if (currentNote == name) return;
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

void ScrollableNoteLabel::mouseDown (const juce::MouseEvent&)
{
    dragSteps = 0;
    dragged   = false;
}

void ScrollableNoteLabel::mouseDrag (const juce::MouseEvent& e)
{
    // Up is positive. Each pxPerStep of travel from the press point is one step.
    const int target = -e.getDistanceFromDragStartY() / pxPerStep;

    if (target != dragSteps)
    {
        if (onNoteChange) onNoteChange (target - dragSteps);
        dragSteps = target;
        dragged   = true;
    }
}

void ScrollableNoteLabel::mouseUp (const juce::MouseEvent& e)
{
    // A plain click: top half = up, bottom half = down. Ignored after a drag so the
    // release doesn't add an extra step.
    if (! dragged && ! e.mouseWasDraggedSinceMouseDown() && onNoteChange)
        onNoteChange (e.y < getHeight() / 2 ? 1 : -1);
}

//==============================================================================
// Randomise menu, shared by the per-track RND buttons and the header RANDOM button. The
// processor bumps uiStateVersion, so the track components resync on their next tick.
static void showRandomiseMenu (PolyrhythmProcessor& proc, juce::Component& target,
                               bool trackA, bool trackB)
{
    using RT = PolyrhythmProcessor::RandomTarget;

    juce::PopupMenu menu;
    menu.addSectionHeader (trackA && trackB ? "Randomise both" : "Randomise");
    menu.addItem (1 + (int) RT::notes,    "Notes");
    menu.addItem (1 + (int) RT::velocity, "Velocity");
    menu.addItem (1 + (int) RT::gate,     "Gate");
    menu.addItem (1 + (int) RT::cutoff,   "LPF");
    menu.addSeparator();
    menu.addItem (1 + (int) RT::all,      "All");

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (target),
                        [&proc, trackA, trackB] (int result)
                        {
                            if (result <= 0) return;
                            const auto what = (RT) (result - 1);
                            if (trackA) proc.randomiseTrack (true,  what);
                            if (trackB) proc.randomiseTrack (false, what);
                        });
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
    makeLabel (gateLabel,      "GATE",                         10.0f, Theme::TextDim);
    makeLabel (velLabel,       "VEL",                          10.0f, Theme::TextDim);
    makeLabel (lpfLabel,       "LPF",                          10.0f, Theme::TextDim);

    // Channel selector — shows the actual parameter value rather than a hardcoded badge
    channelBtn.setColour (juce::TextButton::buttonColourId,  Theme::Accent);
    channelBtn.setColour (juce::TextButton::textColourOnId,  Theme::TextPrimary);
    channelBtn.setColour (juce::TextButton::textColourOffId, Theme::TextPrimary);
    channelBtn.onClick = [this] { showChannelMenu(); };
    addAndMakeVisible (channelBtn);

    randomBtn.setColour (juce::TextButton::buttonColourId,  Theme::BeatBg);
    randomBtn.setColour (juce::TextButton::textColourOnId,  Theme::TextDim);
    randomBtn.setColour (juce::TextButton::textColourOffId, Theme::TextDim);
    randomBtn.setTooltip ("Randomise notes, velocity, gate or LPF on this rhythm");
    randomBtn.onClick = [this] { showRandomiseMenu (processor, randomBtn, trackA, ! trackA); };
    addAndMakeVisible (randomBtn);

    addAndMakeVisible (decBtn);
    addAndMakeVisible (incBtn);
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

    // Gate, velocity and LPF sliders (track-level). The processor copies each parameter
    // into every per-beat value, so dragging and automating behave the same. Colours match
    // the per-beat sliders on the pads.
    auto setUpTrackSlider = [this] (juce::Slider& slider, juce::Colour fill, juce::Colour thumb,
                                    const juce::String& paramId,
                                    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>& attach)
    {
        slider.setSliderStyle (juce::Slider::LinearHorizontal);
        slider.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
        slider.setColour (juce::Slider::trackColourId,      fill);
        slider.setColour (juce::Slider::backgroundColourId, Theme::Border);
        slider.setColour (juce::Slider::thumbColourId,      thumb);
        addAndMakeVisible (slider);
        attach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>
                 (processor.apvts, paramId, slider);
    };

    const juce::String prefix = isA ? "trackA_" : "trackB_";
    setUpTrackSlider (gateSlider, Theme::Accent, Theme::Accent, prefix + "gate", gateAttach);
    setUpTrackSlider (velSlider,  Theme::TextPrimary.withAlpha (0.7f), Theme::TextPrimary,
                      prefix + "velocity", velAttach);
    setUpTrackSlider (lpfSlider,  Theme::CutoffBlue, Theme::CutoffBlue, prefix + "lpf", lpfAttach);


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
        // A per-beat note change moves the track's "all notes" readout out of sync, so
        // refresh it here rather than only on the animation timer
        pads[i]->onChange = [this]
        {
            processor.markBeatStateEdited();
            updateNoteLabel();
            repaint();
        };
        pads[i]->stepNote = [this] (int n, int delta) { return processor.stepNote (n, delta); };
        addAndMakeVisible (*pads[i]);
    }

    updateBeatCountLabel();
    updateNoteLabel();
    updateChannelLabel();
    refreshPads();

    // Initialise fire-count baseline so we don't flash on first load
    lastFireCount = trackA ? processor.trackAFireCount.load()
                           : processor.trackBFireCount.load();
    lastStateVersion = processor.uiStateVersion.load();

    startTimerHz (30);   // 30 Hz animation timer
}

void RhythmTrackComponent::timerCallback()
{
    // A project load or automation changed something we lay out from
    const uint32_t version = processor.uiStateVersion.load();
    if (version != lastStateVersion)
    {
        lastStateVersion = version;
        refreshAll();
    }

    // Check if a new beat has fired since last tick
    const uint64_t current = trackA ? processor.trackAFireCount.load()
                                    : processor.trackBFireCount.load();
    if (current != lastFireCount)
    {
        lastFireCount = current;
        flashBeatIdx  = trackA ? processor.trackACurrentBeat.load()
                               : processor.trackBCurrentBeat.load();
        flashAmount   = 1.0f;
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

    processor.markBeatStateEdited();
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

void RhythmTrackComponent::updateChannelLabel()
{
    const int ch = trackA ? processor.getTrackAChannel() : processor.getTrackBChannel();
    channelBtn.setButtonText (juce::String::formatted ("CH %02d", ch));
}

void RhythmTrackComponent::showChannelMenu()
{
    auto* param = trackA
        ? dynamic_cast<juce::AudioParameterInt*> (processor.apvts.getParameter ("trackA_channel"))
        : dynamic_cast<juce::AudioParameterInt*> (processor.apvts.getParameter ("trackB_channel"));

    if (param == nullptr) return;

    juce::PopupMenu menu;
    const int currentCh = param->get();

    for (int ch = 1; ch <= 16; ++ch)
        menu.addItem (ch, juce::String::formatted ("Channel %d", ch), true, ch == currentCh);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (channelBtn),
                        [this, param] (int result)
                        {
                            if (result <= 0) return;
                            *param = result;
                            updateChannelLabel();
                        });
}

void RhythmTrackComponent::resetNotes()
{
    if (trackA)
        processor.resetTrackANotes();
    else
        processor.resetTrackBNotes();
    processor.markBeatStateEdited();
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

void RhythmTrackComponent::setUiScale (float newScale)
{
    for (auto& pad : pads)
        if (pad != nullptr)
            pad->setUiScale (newScale);
}

void RhythmTrackComponent::refreshAll()
{
    updateBeatCountLabel();
    updateNoteLabel();
    updateChannelLabel();

    for (auto& pad : pads)
    {
        if (pad == nullptr) continue;
        pad->updateVelocitySlider();
        pad->updateGateSlider();
        pad->updateCutoffSlider();
        pad->refreshNoteDisplay();
    }

    refreshPads();
    resized();
    repaint();
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
    channelBtn.setBounds     (w - 66, 8, 58,  20);
    randomBtn.setBounds      (w - 116, 8, 44, 20);

    // Left column: beats row, then note row (scrollable) + reset button
    const int beatsY = 46;
    beatsLabel.setBounds     (12,  beatsY + 4, 44, 16);
    decBtn.setBounds         (58,  beatsY, btnH, btnH);
    beatCountLabel.setBounds (84,  beatsY + 2, 24, 20);
    incBtn.setBounds         (110, beatsY, btnH, btnH);

    const int noteY = beatsY + btnH + 8;
    const int scrollH = 36;
    noteLabel.setBounds       (12,  noteY + 8, 40, 16);
    noteScroller.setBounds    (58,  noteY,     60, scrollH);
    resetNotesBtn.setBounds   (122, noteY + 6, btnH, btnH);

    // Right column: GATE, VEL and LPF stacked beside the beats and note rows
    const int colX   = 164;
    const int rowH   = 22;
    const int labelW = 36;
    int rowY = beatsY;
    for (auto [label, slider] : { std::pair { &gateLabel, &gateSlider },
                                  std::pair { &velLabel,  &velSlider  },
                                  std::pair { &lpfLabel,  &lpfSlider  } })
    {
        label->setBounds  (colX,          rowY + 3, labelW,                 16);
        slider->setBounds (colX + labelW, rowY,     w - colX - labelW - 12, rowH);
        rowY += rowH + 2;
    }

    // Beat pads — fill remaining height
    const int padAreaY = juce::jmax (noteY + scrollH, rowY) + 12;
    const int padAreaH = H - padAreaY - 10;
    const int count    = trackA ? processor.getTrackABeatCount()
                                : processor.getTrackBBeatCount();
    const int gap      = count > 8 ? 3 : 5;
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
// PolyrhythmContent
//==============================================================================
PolyrhythmContent::PolyrhythmContent (PolyrhythmProcessor& p)
    : audioProcessor (p), trackAComp (p, true), trackBComp (p, false)
{
    setSize (EDITOR_BASE_W, EDITOR_BASE_H);

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

    auto styleHeaderBtn = [this] (juce::TextButton& btn)
    {
        btn.setColour (juce::TextButton::buttonColourId,   Theme::BeatBg);
        btn.setColour (juce::TextButton::buttonOnColourId, Theme::Accent);
        btn.setColour (juce::TextButton::textColourOnId,   Theme::TextPrimary);
        btn.setColour (juce::TextButton::textColourOffId,  Theme::TextDim);
        addAndMakeVisible (btn);
    };

    // Scale lock
    styleHeaderBtn (scaleBtn);
    scaleBtn.setClickingTogglesState (true);
    scaleBtn.setTooltip ("Lock every note to the chosen scale. Turning it off keeps the notes.");
    scaleBtn.onClick = [this] { applyScaleFromUi(); };

    for (auto* box : { &scaleRootBox, &scaleTypeBox })
    {
        box->setColour (juce::ComboBox::backgroundColourId, Theme::BeatBg);
        box->setColour (juce::ComboBox::textColourId,       Theme::TextPrimary);
        box->setColour (juce::ComboBox::outlineColourId,    Theme::Border);
        box->setColour (juce::ComboBox::arrowColourId,      Theme::TextDim);
        box->onChange = [this] { applyScaleFromUi(); };
        addAndMakeVisible (*box);
    }

    const juce::StringArray noteNames { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    for (int i = 0; i < noteNames.size(); ++i)
        scaleRootBox.addItem (noteNames[i], i + 1);

    const auto& scales = PolyrhythmProcessor::getScales();
    for (int i = 0; i < (int) scales.size(); ++i)
        scaleTypeBox.addItem (scales[(size_t) i].name, i + 1);

    updateScaleControls();

    // Global reset, matching the web version's RESET button
    styleHeaderBtn (resetAllBtn);
    resetAllBtn.onClick = [this] { resetAll(); };

    styleHeaderBtn (randomAllBtn);
    randomAllBtn.setTooltip ("Randomise notes, velocity, gate or LPF on both rhythms");
    randomAllBtn.onClick = [this] { showRandomiseMenu (audioProcessor, randomAllBtn, true, true); };

    // CC74 output toggle
    styleHeaderBtn (ccToggleBtn);
    ccToggleBtn.setClickingTogglesState (true);
    ccToggleBtn.setToggleState (audioProcessor.sendCutoffCC.load(), juce::dontSendNotification);
    ccToggleBtn.setTooltip ("Send per-beat LPF as MIDI CC74. Off keeps recorded regions clean.");
    ccToggleBtn.onClick = [this] {
        audioProcessor.sendCutoffCC.store (ccToggleBtn.getToggleState());
        audioProcessor.markBeatStateEdited();
    };

    // Direct MIDI output device picker
    styleHeaderBtn (midiOutBtn);
    midiOutBtn.onClick = [this] { showMidiOutMenu(); };

    addAndMakeVisible (trackAComp);
    addAndMakeVisible (trackBComp);

    updateMidiOutButton();
    lastStateVersion = audioProcessor.uiStateVersion.load();
    startTimerHz (10);   // BPM readout, MIDI status, host notification
}

PolyrhythmContent::~PolyrhythmContent() { stopTimer(); }

void PolyrhythmContent::timerCallback()
{
    const double bpm = audioProcessor.currentBpm.load();
    if (bpm > 0.0)
        bpmValueLabel.setText (juce::String ((int)std::round (bpm)),
                               juce::dontSendNotification);

    // The device can disappear (IAC bus disabled, interface unplugged) while we're open
    if (midiOutConnected != audioProcessor.getMidiOutputIdentifier().isNotEmpty())
        updateMidiOutButton();

    // A project load can change the CC74 setting and the MIDI port under us
    const uint32_t version = audioProcessor.uiStateVersion.load();
    if (version != lastStateVersion)
    {
        lastStateVersion = version;
        ccToggleBtn.setToggleState (audioProcessor.sendCutoffCC.load(), juce::dontSendNotification);
        updateScaleControls();
        updateMidiOutButton();
    }

    // Beat edits are not parameter changes, so the host has to be told separately or it
    // may not mark the project as modified. Batched here so a slider drag is one message
    // per tick rather than one per pixel.
    if (audioProcessor.takeBeatStateEdited())
        audioProcessor.updateHostDisplay (juce::AudioProcessor::ChangeDetails{}
                                              .withNonParameterStateChanged (true));
}

void PolyrhythmContent::updateMidiOutButton()
{
    const juce::String name = audioProcessor.getMidiOutputName();
    midiOutConnected = audioProcessor.getMidiOutputIdentifier().isNotEmpty();

    midiOutBtn.setButtonText (midiOutConnected ? name : juce::String ("MIDI OUT"));
    midiOutBtn.setColour (juce::TextButton::textColourOffId,
                          midiOutConnected ? Theme::MidiGreen : Theme::TextDim);
    repaint();
}

void PolyrhythmContent::showMidiOutMenu()
{
    const auto devices = PolyrhythmProcessor::getMidiOutputDevices();
    const auto current = audioProcessor.getMidiOutputIdentifier();

    juce::PopupMenu menu;
    menu.addItem (1, "None", true, current.isEmpty());

    if (devices.isEmpty())
    {
        menu.addSeparator();
        menu.addItem (2, "No MIDI outputs found", false, false);
    }
    else
    {
        menu.addSeparator();

        for (int i = 0; i < devices.size(); ++i)
            menu.addItem (i + 10, devices[i].name, true,
                          devices[i].identifier == current);
    }

    menu.addSeparator();
    menu.addItem (3, "Enable the IAC Driver in Audio MIDI Setup to record into a DAW track",
                  false, false);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (midiOutBtn),
                        [this, devices] (int result)
                        {
                            if (result <= 0 || result == 2 || result == 3) return;

                            if (result == 1)
                                audioProcessor.setMidiOutputDevice ({});
                            else
                                audioProcessor.setMidiOutputDevice (devices[result - 10].identifier);

                            audioProcessor.markBeatStateEdited();
                            updateMidiOutButton();
                        });
}

void PolyrhythmContent::applyScaleFromUi()
{
    audioProcessor.scaleEnabled.store (scaleBtn.getToggleState());
    audioProcessor.scaleRoot.store (juce::jmax (0, scaleRootBox.getSelectedId() - 1));
    audioProcessor.scaleType.store (juce::jmax (0, scaleTypeBox.getSelectedId() - 1));

    // Turning the lock on, or changing key while it is on, snaps every note into the scale
    audioProcessor.snapAllNotesToScale();
    audioProcessor.markBeatStateEdited();

    updateScaleControls();
    trackAComp.refreshAll();
    trackBComp.refreshAll();
}

void PolyrhythmContent::updateScaleControls()
{
    const bool on = audioProcessor.scaleEnabled.load();
    scaleBtn.setToggleState (on, juce::dontSendNotification);
    scaleRootBox.setSelectedId (audioProcessor.scaleRoot.load() + 1, juce::dontSendNotification);
    scaleTypeBox.setSelectedId (audioProcessor.scaleType.load() + 1, juce::dontSendNotification);

    // Dim the key while the lock is off, as the web version does
    for (auto* box : { &scaleRootBox, &scaleTypeBox })
        box->setColour (juce::ComboBox::textColourId, on ? Theme::TextPrimary : Theme::TextDim);
}

void PolyrhythmContent::setUiScale (float newScale)
{
    trackAComp.setUiScale (newScale);
    trackBComp.setUiScale (newScale);
}

void PolyrhythmContent::resetAll()
{
    auto setParam = [this] (const juce::String& id, float value)
    {
        if (auto* p = audioProcessor.apvts.getParameter (id))
            p->setValueNotifyingHost (p->convertTo0to1 (value));
    };

    setParam ("trackA_beats",   4.0f);
    setParam ("trackB_beats",   3.0f);
    setParam ("trackA_channel", 1.0f);
    setParam ("trackB_channel", 2.0f);
    setParam ("swing",          0.0f);
    setParam ("trackA_gate",    0.5f);
    setParam ("trackB_gate",    0.5f);
    setParam ("trackA_velocity", 0.8f);
    setParam ("trackB_velocity", 0.8f);
    setParam ("trackA_lpf",     1.0f);
    setParam ("trackB_lpf",     1.0f);
    setParam ("probability",    1.0f);

    // Scale lock off before the notes reset, so they land on the plain defaults
    audioProcessor.scaleEnabled.store (false);
    audioProcessor.scaleRoot.store (0);
    audioProcessor.scaleType.store (0);
    updateScaleControls();

    for (int i = 0; i < MAX_BEATS; ++i)
    {
        audioProcessor.trackAActive[i].store (true);
        audioProcessor.trackBActive[i].store (true);
        audioProcessor.trackAVelocity[i].store (0.8f);
        audioProcessor.trackBVelocity[i].store (0.8f);
        audioProcessor.trackACutoff[i].store (1.0f);
        audioProcessor.trackBCutoff[i].store (1.0f);
        audioProcessor.trackAGate[i].store (0.5f);
        audioProcessor.trackBGate[i].store (0.5f);
    }

    audioProcessor.resetTrackANotes();
    audioProcessor.resetTrackBNotes();

    audioProcessor.sendCutoffCC.store (false);
    ccToggleBtn.setToggleState (false, juce::dontSendNotification);
    audioProcessor.markBeatStateEdited();

    trackAComp.refreshAll();
    trackBComp.refreshAll();
}

void PolyrhythmContent::paint (juce::Graphics& g)
{
    const int W = getWidth();
    const int H = getHeight();

    g.fillAll (Theme::BG);

    // Header bar
    g.setColour (Theme::PanelDark);
    g.fillRect (0, 0, W, 44);
    g.setColour (Theme::Border);
    g.drawLine (0, 44, (float) W, 44, 1.0f);

    // MIDI status dot next to the device button
    g.setColour (midiOutConnected ? Theme::MidiGreen : Theme::Border);
    g.fillEllipse ((float) midiOutBtn.getX() - 12.0f, 18.0f, 8.0f, 8.0f);

    // Second header row (scale, swing, prob)
    g.setColour (Theme::Border);
    g.drawLine (0, (float) toolbarBottom, (float) W, (float) toolbarBottom, 1.0f);

    // Vertical divider between tracks
    g.setColour (Theme::Accent);
    g.fillRect (W / 2 - 1, toolbarBottom + 6, 2, H - toolbarBottom - 10);
}

void PolyrhythmContent::resized()
{
    const int W = getWidth();
    const int H = getHeight();

    // Header. The right-hand group is packed right to left so the BPM readout, the PROB
    // slider and the SWING slider cannot overlap each other — the PROB slider used to sit
    // straight on top of the BPM digits.
    titleLabel.setBounds  (12,  8, 200, 28);
    resetAllBtn.setBounds  (220, 10, 56, 24);
    randomAllBtn.setBounds (282, 10, 66, 24);
    ccToggleBtn.setBounds  (354, 10, 52, 24);
    midiOutBtn.setBounds   (424, 10, 150, 24);

    int right = W - 12;

    bpmUnitLabel.setBounds  (right - 32, 20, 32, 16);   right -= 32 + 4;
    bpmValueLabel.setBounds (right - 74,  4, 74, 36);

    // Second row: scale lock on the left, SWING and PROB on the right
    const int rowY = 50;
    scaleBtn.setBounds     (12,  rowY, 56,  22);
    scaleRootBox.setBounds (74,  rowY, 56,  22);
    scaleTypeBox.setBounds (136, rowY, 160, 22);

    right = W - 12;
    probSlider.setBounds  (right - 200, rowY + 1, 200, 20); right -= 200 + 24;
    swingSlider.setBounds (right - 200, rowY + 1, 200, 20);

    // Track panels fill everything below the header rows
    const int trackY = toolbarBottom + 6;
    const int trackH = H - trackY - 4;
    const int halfW  = W / 2 - 2;
    trackAComp.setBounds (2,          trackY, halfW, trackH);
    trackBComp.setBounds (halfW + 4,  trackY, halfW, trackH);
}

//==============================================================================
// PolyrhythmEditor
//==============================================================================
PolyrhythmEditor::PolyrhythmEditor (PolyrhythmProcessor& p)
    : AudioProcessorEditor (&p), audioProcessor (p), content (p)
{
    addAndMakeVisible (content);

    // The layout inside `content` is fixed at the base size and scaled by a transform, so
    // resizing genuinely enlarges the controls rather than just adding empty space.
    // setResizeLimits() must come first — it is what installs the default constrainer that
    // the aspect ratio is then set on.
    setResizeLimits (EDITOR_BASE_W / 2, EDITOR_BASE_H / 2,
                     EDITOR_BASE_W * 2, EDITOR_BASE_H * 2);
    setResizable (true, true);

    if (auto* constrainer = getConstrainer())
        constrainer->setFixedAspectRatio ((double) EDITOR_BASE_W / (double) EDITOR_BASE_H);

    setSize (audioProcessor.editorWidth.load(), audioProcessor.editorHeight.load());
}

PolyrhythmEditor::~PolyrhythmEditor() = default;

void PolyrhythmEditor::paint (juce::Graphics& g)
{
    // Only ever visible in the sliver left by aspect-ratio rounding
    g.fillAll (Theme::BG);
}

void PolyrhythmEditor::resized()
{
    const float scale = juce::jmin ((float) getWidth()  / (float) EDITOR_BASE_W,
                                    (float) getHeight() / (float) EDITOR_BASE_H);

    content.setBounds (0, 0, EDITOR_BASE_W, EDITOR_BASE_H);
    content.setTransform (juce::AffineTransform::scale (scale));

    // Child bounds are unchanged by the transform, so tell the pads how big they now look
    content.setUiScale (scale);

    audioProcessor.editorWidth.store (getWidth());
    audioProcessor.editorHeight.store (getHeight());
}
