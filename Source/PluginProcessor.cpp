#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <algorithm>

//==============================================================================
/*  The same sources build two plugins:

      - a MIDI effect (AU 'aumi') for Logic's MIDI FX slot, with no audio buses
      - an instrument (AU 'aumu') for the Instrument slot, which needs an audio bus even
        though this plugin only ever emits MIDI and leaves the buffer silent

    JucePlugin_IsMidiEffect comes from the IS_MIDI_EFFECT flag on each CMake target.
*/
PolyrhythmProcessor::PolyrhythmProcessor()
    : AudioProcessor (
       #if JucePlugin_IsMidiEffect
          BusesProperties()
       #else
          BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)
       #endif
      ),
      apvts (*this, nullptr, "Parameters", createParameterLayout())
{
    for (auto* id : { "trackA_beats", "trackB_beats", "trackA_channel", "trackB_channel",
                      "trackA_gate", "trackB_gate" })
        apvts.addParameterListener (id, this);

    for (size_t i = 0; i < MAX_BEATS; ++i)
    {
        trackAActive[i]   = true;
        trackBActive[i]   = true;
        trackAVelocity[i] = 0.8f;
        trackBVelocity[i] = 0.8f;
        trackANotes[i]    = 36;   // C2 default for track A
        trackBNotes[i]    = 38;   // D2 default for track B
        trackACutoff[i]   = 1.0f; // LP filter wide open
        trackBCutoff[i]   = 1.0f;
        trackAGate[i]     = 0.5f;
        trackBGate[i]     = 0.5f;
    }

    resetCutoffCC();
}

PolyrhythmProcessor::~PolyrhythmProcessor()
{
    for (auto* id : { "trackA_beats", "trackB_beats", "trackA_channel", "trackB_channel",
                      "trackA_gate", "trackB_gate" })
        apvts.removeParameterListener (id, this);

    midiOutSender.stopTimer();
    setMidiOutputDevice ({});
}

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout
PolyrhythmProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    // Version hints let hosts tell parameter sets apart if the list ever changes. Every
    // parameter here dates from version 1; new ones should use the plugin version they ship in.
    using PID = juce::ParameterID;

    params.push_back (std::make_unique<juce::AudioParameterInt>  (PID { "trackA_beats",   1 }, "Track A Beats",          1,  16,  4));
    params.push_back (std::make_unique<juce::AudioParameterInt>  (PID { "trackB_beats",   1 }, "Track B Beats",          1,  16,  3));
    // Track B defaults to channel 2 so the two tracks are separable downstream — the UI
    // has always shown "CH 02" for track B, but the default used to be 1 like track A,
    // so both tracks landed on the same channel.
    params.push_back (std::make_unique<juce::AudioParameterInt>  (PID { "trackA_channel", 1 }, "Track A MIDI Channel",   1,  16,  1));
    params.push_back (std::make_unique<juce::AudioParameterInt>  (PID { "trackB_channel", 1 }, "Track B MIDI Channel",   1,  16,  2));
    // Per-beat notes are stored in the beat state ValueTree, not as automatable params
    params.push_back (std::make_unique<juce::AudioParameterFloat>(PID { "swing",          1 }, "Swing",                  0.0f, 0.5f,  0.0f));
    params.push_back (std::make_unique<juce::AudioParameterFloat>(PID { "trackA_gate",    1 }, "Track A Gate",           0.01f, 1.0f, 0.5f));
    params.push_back (std::make_unique<juce::AudioParameterFloat>(PID { "trackB_gate",    1 }, "Track B Gate",           0.01f, 1.0f, 0.5f));
    params.push_back (std::make_unique<juce::AudioParameterFloat>(PID { "probability",    1 }, "Probability",            0.0f,  1.0f,  1.0f));

    return { params.begin(), params.end() };
}

//==============================================================================
/*  Can run on the audio thread under automation, so this only stores atomics.

    The track gate sets every per-beat gate on its track, whether it moves from the
    editor or from host automation. On a project load the parameters are restored before
    the beat tree, so saved per-beat gates still win over the track gate.
*/
void PolyrhythmProcessor::parameterChanged (const juce::String& parameterID, float newValue)
{
    if (parameterID == "trackA_gate" || parameterID == "trackB_gate")
    {
        auto& gates = parameterID == "trackA_gate" ? trackAGate : trackBGate;
        for (auto& g : gates)
            g.store (newValue);
    }

    uiStateVersion.fetch_add (1);
}

//==============================================================================
// Parameter helpers
int   PolyrhythmProcessor::getTrackABeatCount() const { return (int)*apvts.getRawParameterValue ("trackA_beats"); }
int   PolyrhythmProcessor::getTrackBBeatCount() const { return (int)*apvts.getRawParameterValue ("trackB_beats"); }
int   PolyrhythmProcessor::getTrackAChannel()   const { return (int)*apvts.getRawParameterValue ("trackA_channel"); }
int   PolyrhythmProcessor::getTrackBChannel()   const { return (int)*apvts.getRawParameterValue ("trackB_channel"); }
int   PolyrhythmProcessor::getTrackANote()      const { return trackANotes[0].load(); }
int   PolyrhythmProcessor::getTrackBNote()      const { return trackBNotes[0].load(); }

void  PolyrhythmProcessor::shiftTrackANotes (int semitones)
{
    for (auto& n : trackANotes)
        n.store (juce::jlimit (0, 127, n.load() + semitones));
}
void  PolyrhythmProcessor::shiftTrackBNotes (int semitones)
{
    for (auto& n : trackBNotes)
        n.store (juce::jlimit (0, 127, n.load() + semitones));
}
void  PolyrhythmProcessor::resetTrackANotes()
{
    for (auto& n : trackANotes)
        n.store (36);   // C2
}
void  PolyrhythmProcessor::resetTrackBNotes()
{
    for (auto& n : trackBNotes)
        n.store (38);   // D2
}
float PolyrhythmProcessor::getSwing()           const { return *apvts.getRawParameterValue ("swing"); }
float PolyrhythmProcessor::getTrackAGate()      const { return *apvts.getRawParameterValue ("trackA_gate"); }
float PolyrhythmProcessor::getTrackBGate()      const { return *apvts.getRawParameterValue ("trackB_gate"); }
float PolyrhythmProcessor::getProbability()     const { return *apvts.getRawParameterValue ("probability"); }

//==============================================================================
// Direct MIDI output device
juce::Array<juce::MidiDeviceInfo> PolyrhythmProcessor::getMidiOutputDevices()
{
    return juce::MidiOutput::getAvailableDevices();
}

void PolyrhythmProcessor::setMidiOutputDevice (const juce::String& identifier)
{
    std::unique_ptr<juce::MidiOutput> newDevice;

    if (identifier.isNotEmpty())
        newDevice = juce::MidiOutput::openDevice (identifier);

    std::unique_ptr<juce::MidiOutput> previous;
    {
        const juce::ScopedLock sl (midiOutLock);
        previous          = std::move (midiOutDevice);
        midiOutDevice     = std::move (newDevice);
        midiOutIdentifier = (midiOutDevice != nullptr) ? identifier : juce::String();
    }

    const bool active = midiOutIdentifier.isNotEmpty();
    midiOutActive.store (active, std::memory_order_relaxed);

    if (active)
    {
        // 1 ms drain. Messages are held until their timestamp, so this is also the timing
        // resolution of the external port. The IAC bus adds jitter of its own, so chasing
        // tighter than this buys nothing.
        if (! midiOutSender.isTimerRunning())
            midiOutSender.startTimer (1);
    }
    else
    {
        midiOutSender.stopTimer();
        drainMidiOutQueue (true);   // clear anything still queued
    }

    previous.reset();
}

//==============================================================================
void PolyrhythmProcessor::queueForExternalMidiOut (const juce::MidiBuffer& generated,
                                                   int numSamples)
{
    if (! midiOutActive.load (std::memory_order_relaxed))
    {
        midiClockValid = false;
        return;
    }

    // Place this block on the wall clock. Every block advances the clock, including empty
    // ones, or the spacing between notes in different blocks would be lost.
    const double nowMs    = juce::Time::getMillisecondCounterHiRes();
    const double msPerSmp = 1000.0 / sampleRateVal;

    if (! midiClockValid || nextBlockMs < nowMs || nextBlockMs - nowMs > maxMidiLeadMs)
        nextBlockMs = nowMs;

    midiClockValid = true;
    const double blockStartMs = nextBlockMs;
    nextBlockMs += juce::jmax (0, numSamples) * msPerSmp;

    for (const auto meta : generated)
    {
        if (meta.numBytes < 1 || meta.numBytes > 3)
            continue;

        int start1, size1, start2, size2;
        midiOutFifo.prepareToWrite (1, start1, size1, start2, size2);

        // Full queue: drop rather than block the audio thread
        if (size1 + size2 < 1)
            return;

        auto& slot = midiOutQueue[(size_t) (size1 > 0 ? start1 : start2)];
        std::memcpy (slot.bytes, meta.data, (size_t) meta.numBytes);
        slot.size     = (juce::uint8) meta.numBytes;
        slot.sendAtMs = blockStartMs + meta.samplePosition * msPerSmp;

        midiOutFifo.finishedWrite (1);
    }
}

/*  Sends every queued message whose time has come. The queue is in time order, so it
    stops at the first message that is still in the future and leaves the rest queued.
*/
void PolyrhythmProcessor::drainMidiOutQueue (bool discardAll)
{
    int start1, size1, start2, size2;
    midiOutFifo.prepareToRead (midiOutFifo.getNumReady(), start1, size1, start2, size2);

    if (discardAll)
    {
        midiOutFifo.finishedRead (size1 + size2);
        return;
    }

    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    int sent = 0;

    if (size1 + size2 > 0)
    {
        const juce::ScopedLock sl (midiOutLock);

        auto sendRange = [&] (int start, int size)
        {
            for (int i = 0; i < size; ++i)
            {
                const auto& slot = midiOutQueue[(size_t) (start + i)];

                if (slot.sendAtMs > nowMs)
                    return false;

                if (midiOutDevice != nullptr && slot.size > 0)
                    midiOutDevice->sendMessageNow (juce::MidiMessage (slot.bytes, (int) slot.size));

                ++sent;
            }

            return true;
        };

        if (sendRange (start1, size1))
            sendRange (start2, size2);
    }

    midiOutFifo.finishedRead (sent);
}

juce::String PolyrhythmProcessor::getMidiOutputIdentifier() const
{
    const juce::ScopedLock sl (midiOutLock);
    return midiOutIdentifier;
}

juce::String PolyrhythmProcessor::getMidiOutputName() const
{
    const juce::ScopedLock sl (midiOutLock);
    return midiOutDevice != nullptr ? midiOutDevice->getName() : juce::String();
}

//==============================================================================
void PolyrhythmProcessor::prepareToPlay (double sr, int)
{
    sampleRateVal = sr;

    // Reserve up front so the audio thread never allocates when scheduling note-offs.
    // Worst case is every beat of both tracks outstanding at once.
    pendingNoteOffs.clear();
    pendingNoteOffs.reserve (MAX_BEATS * 2);

    ppqCursorValid = false;
    wasPlaying     = false;
    resetCutoffCC();
}

void PolyrhythmProcessor::releaseResources() {}

bool PolyrhythmProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    if (layouts.getMainInputChannelSet() != juce::AudioChannelSet::disabled())
        return false;

   #if JucePlugin_IsMidiEffect
    // A MIDI effect carries no audio at all
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::disabled();
   #else
    // The instrument build needs an output bus to load, though it stays silent
    const auto out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::stereo()
        || out == juce::AudioChannelSet::mono()
        || out == juce::AudioChannelSet::disabled();
   #endif
}

//==============================================================================
void PolyrhythmProcessor::resetCutoffCC()
{
    lastCutoffCC.fill (-1);
}

void PolyrhythmProcessor::flushPendingNoteOffs (juce::MidiBuffer& midiOut, int sampleOffset)
{
    for (auto& noff : pendingNoteOffs)
        midiOut.addEvent (juce::MidiMessage::noteOff (noff.channel, noff.note), sampleOffset);

    pendingNoteOffs.clear();
}

//==============================================================================
void PolyrhythmProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                         juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();

    const int numSamples = buffer.getNumSamples();

    // Everything we generate lands here first. midiMessages arrives holding the host's
    // input, which is passed through to the instrument downstream but must never be fed
    // back to the external port.
    generatedMidi.clear();

    auto* playHead = getPlayHead();
    auto  posOpt   = playHead != nullptr ? playHead->getPosition()
                                         : juce::Optional<juce::AudioPlayHead::PositionInfo>();

    const bool playing = posOpt.hasValue() && posOpt->getIsPlaying();
    transportPlaying.store (playing);

    if (posOpt.hasValue())
        if (auto bpmOpt = posOpt->getBpm())
            currentBpm.store (*bpmOpt);

    if (! playing)
    {
        if (wasPlaying)
        {
            // Close every note we still have hanging, then belt-and-braces all-notes-off
            // on both channels so nothing sticks in the instrument downstream
            flushPendingNoteOffs (generatedMidi, 0);
            generatedMidi.addEvent (juce::MidiMessage::allNotesOff (getTrackAChannel()), 0);
            generatedMidi.addEvent (juce::MidiMessage::allNotesOff (getTrackBChannel()), 0);

            wasPlaying     = false;
            ppqCursorValid = false;
            resetCutoffCC();
        }

        publishGeneratedMidi (midiMessages, numSamples);
        return;
    }

    wasPlaying = true;

    auto ppqOpt = posOpt->getPpqPosition();
    if (! ppqOpt.hasValue())
    {
        publishGeneratedMidi (midiMessages, numSamples);
        return;
    }

    const double bpm           = currentBpm.load();
    const double blockStartPpq = *ppqOpt;
    const double ppqPerSample  = bpm / (60.0 * sampleRateVal);
    const double blockEndPpq   = blockStartPpq + numSamples * ppqPerSample;

    // Bar length in PPQ from the time signature (default 4/4)
    int numerator = 4, denominator = 4;
    if (auto tsOpt = posOpt->getTimeSignature())
    {
        numerator   = tsOpt->numerator;
        denominator = tsOpt->denominator;
    }
    const double barLengthPpq = (4.0 * numerator) / denominator;

    // Any bar line the host reports. Bars are laid out from here rather than from PPQ zero,
    // which is only a bar line until a pickup bar or a time signature change moves the grid.
    double barAnchorPpq = 0.0;
    if (auto barStartOpt = posOpt->getPpqPositionOfLastBarStart())
        barAnchorPpq = *barStartOpt;

    // Work out the PPQ window to search for beats. Continuing from where the last block
    // finished closes the gaps and overlaps between host-reported block positions; a gap
    // would drop beats and an overlap would fire them twice.
    double windowStartPpq = blockStartPpq;

    if (! ppqCursorValid || std::abs (blockStartPpq - nextPpq) > ppqDiscontinuity)
    {
        // Transport looped or was relocated — resync rather than trying to catch up over
        // the jump, and drop note-offs that belong to the old playback position. The
        // cursor has to be moved too: on a loop back it is ahead of the new position, and
        // leaving it there would stall the sequencer until playback caught up again.
        flushPendingNoteOffs (generatedMidi, 0);
        resetCutoffCC();
        ppqCursorValid = true;
        nextPpq        = blockStartPpq;
    }
    else
    {
        windowStartPpq = nextPpq;
    }

    // Emit note-offs that came due during this block
    {
        size_t keep = 0;
        for (size_t i = 0; i < pendingNoteOffs.size(); ++i)
        {
            auto& noff = pendingNoteOffs[i];

            if (noff.samplesRemaining < numSamples)
            {
                generatedMidi.addEvent (juce::MidiMessage::noteOff (noff.channel, noff.note),
                                        noff.samplesRemaining);
            }
            else
            {
                pendingNoteOffs[keep++] = { noff.samplesRemaining - numSamples,
                                            noff.channel, noff.note };
            }
        }
        pendingNoteOffs.resize (keep);
    }

    if (numSamples > 0 && blockEndPpq > windowStartPpq)
    {
        processTrack (true,  barLengthPpq, barAnchorPpq, windowStartPpq, blockEndPpq,
                      blockStartPpq, ppqPerSample, numSamples, generatedMidi);
        processTrack (false, barLengthPpq, barAnchorPpq, windowStartPpq, blockEndPpq,
                      blockStartPpq, ppqPerSample, numSamples, generatedMidi);
    }

    nextPpq = juce::jmax (nextPpq, blockEndPpq);

    publishGeneratedMidi (midiMessages, numSamples);
}

//==============================================================================
/*  Generated notes go two places: into the host's buffer, where they join the
    pass-through input on the way to the instrument, and onto the external port. Only the
    generated half reaches the port — see queueForExternalMidiOut.
*/
void PolyrhythmProcessor::publishGeneratedMidi (juce::MidiBuffer& hostBuffer, int numSamples)
{
    queueForExternalMidiOut (generatedMidi, numSamples);

    if (generatedMidi.isEmpty())
        return;

    hostBuffer.addEvents (generatedMidi, 0, numSamples > 0 ? numSamples : -1, 0);
}

//==============================================================================
void PolyrhythmProcessor::processTrack (bool isTrackA,
                                         double barLengthPpq,
                                         double barAnchorPpq,
                                         double windowStartPpq,
                                         double windowEndPpq,
                                         double blockStartPpq,
                                         double ppqPerSample,
                                         int    numSamples,
                                         juce::MidiBuffer& midiOut)
{
    const int   beatCount = isTrackA ? getTrackABeatCount() : getTrackBBeatCount();
    const int   channel   = isTrackA ? getTrackAChannel()   : getTrackBChannel();
    const float prob      = getProbability();
    const float swing     = getSwing();

    auto& activeArr = isTrackA ? trackAActive   : trackBActive;
    auto& velArr    = isTrackA ? trackAVelocity : trackBVelocity;
    auto& noteArr   = isTrackA ? trackANotes    : trackBNotes;
    auto& cutoffArr = isTrackA ? trackACutoff   : trackBCutoff;
    auto& gateArr   = isTrackA ? trackAGate     : trackBGate;

    const double beatInterval = barLengthPpq / std::max (1, beatCount);

    // Search the bar containing the start of the window, plus the next one, so beats that
    // land just past a bar line are still found
    const double barStart = barAnchorPpq
                          + std::floor ((windowStartPpq - barAnchorPpq) / barLengthPpq) * barLengthPpq;

    // Swing pushes odd beats late, so the gap from one beat to the next is not always one
    // beat interval. Beat offsets within the bar, with the next bar's downbeat at the end.
    auto beatOffsetInBar = [&] (int beat)
    {
        if (beat >= beatCount)
            return barLengthPpq;

        double offset = beat * beatInterval;
        if (beat % 2 == 1)
            offset += swing * beatInterval;

        return offset;
    };

    for (int barOffset = 0; barOffset <= 1; ++barOffset)
    {
        const double currentBarStart = barStart + barOffset * barLengthPpq;

        for (size_t beat = 0; beat < (size_t) beatCount; ++beat)
        {
            if (! activeArr[beat].load()) continue;

            // Apply swing: push odd-numbered beats (1, 3, 5…) forward in time
            const double beatOffset = beatOffsetInBar ((int) beat);
            const double beatPpq    = currentBarStart + beatOffset;

            if (beatPpq < windowStartPpq || beatPpq >= windowEndPpq)
                continue;

            // Probability gate
            if (prob < 1.0f && random.nextFloat() > prob) continue;

            // Beats recovered from a gap sit before this block and clamp to sample 0
            int sampleOffset = (int) ((beatPpq - blockStartPpq) / ppqPerSample);
            sampleOffset = juce::jlimit (0, numSamples - 1, sampleOffset);

            const int velInt   = juce::jlimit (1, 127, (int) (velArr[beat].load() * 127.0f));
            const int beatNote = juce::jlimit (0, 127, noteArr[beat].load());

            // Optional LP cutoff as MIDI CC74 (Brightness), only when the value actually
            // changed — sending it on every note floods recorded regions
            if (sendCutoffCC.load())
            {
                const int ccVal = juce::jlimit (0, 127, (int) (cutoffArr[beat].load() * 127.0f));

                if (ccVal != lastCutoffCC[(size_t) channel])
                {
                    lastCutoffCC[(size_t) channel] = ccVal;
                    midiOut.addEvent (juce::MidiMessage::controllerEvent (channel, 74, ccVal),
                                      sampleOffset);
                }
            }

            midiOut.addEvent (juce::MidiMessage::noteOn (channel, beatNote, (juce::uint8) velInt),
                              sampleOffset);

            // Signal the UI which beat just fired (for pulse animation)
            if (isTrackA)
            {
                trackACurrentBeat.store ((int) beat, std::memory_order_relaxed);
                trackAFireCount.fetch_add (1, std::memory_order_relaxed);
            }
            else
            {
                trackBCurrentBeat.store ((int) beat, std::memory_order_relaxed);
                trackBFireCount.fetch_add (1, std::memory_order_relaxed);
            }

            // Per-beat gate, capped just under the gap to the next beat so a note always
            // closes before the same note retriggers. Under swing that gap can be shorter
            // than a beat interval.
            const double gapToNext   = beatOffsetInBar ((int) beat + 1) - beatOffset;
            const float  beatGateVal = juce::jlimit (0.01f, 1.0f, gateArr[beat].load());
            const double gatePpq     = std::min ((double) beatGateVal * beatInterval,
                                                 0.99 * gapToNext);
            const int    gateSamples = std::max (1, (int) (gatePpq / ppqPerSample));

            const int noteOffSample = sampleOffset + gateSamples;

            if (noteOffSample < numSamples)
            {
                midiOut.addEvent (juce::MidiMessage::noteOff (channel, beatNote), noteOffSample);
            }
            else if (pendingNoteOffs.size() < pendingNoteOffs.capacity())
            {
                // Guarded against growth so the audio thread never allocates here
                pendingNoteOffs.push_back ({ noteOffSample - numSamples, channel, beatNote });
            }
        }
    }
}

//==============================================================================
juce::AudioProcessorEditor* PolyrhythmProcessor::createEditor()
{
    return new PolyrhythmEditor (*this);
}

//==============================================================================
void PolyrhythmProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();

    // Loading state hands the whole tree to apvts, beat tree included, so a copy from the
    // last load can still be in there. Drop it, or the stale copy is the one read back.
    for (auto old = state.getChildWithName ("BeatStates"); old.isValid();
         old = state.getChildWithName ("BeatStates"))
        state.removeChild (old, nullptr);

    // Append beat grid data as a child tree
    juce::ValueTree beatTree ("BeatStates");
    for (size_t i = 0; i < MAX_BEATS; ++i)
    {
        beatTree.setProperty ("aActive" + juce::String ((int)i), trackAActive[i].load(),   nullptr);
        beatTree.setProperty ("aVel"    + juce::String ((int)i), trackAVelocity[i].load(), nullptr);
        beatTree.setProperty ("aNote"   + juce::String ((int)i), trackANotes[i].load(),    nullptr);
        beatTree.setProperty ("aCutoff" + juce::String ((int)i), trackACutoff[i].load(),   nullptr);
        beatTree.setProperty ("aGate"   + juce::String ((int)i), trackAGate[i].load(),     nullptr);
        beatTree.setProperty ("bActive" + juce::String ((int)i), trackBActive[i].load(),   nullptr);
        beatTree.setProperty ("bVel"    + juce::String ((int)i), trackBVelocity[i].load(), nullptr);
        beatTree.setProperty ("bNote"   + juce::String ((int)i), trackBNotes[i].load(),    nullptr);
        beatTree.setProperty ("bCutoff" + juce::String ((int)i), trackBCutoff[i].load(),   nullptr);
        beatTree.setProperty ("bGate"   + juce::String ((int)i), trackBGate[i].load(),     nullptr);
    }
    beatTree.setProperty ("sendCutoffCC",  sendCutoffCC.load(),      nullptr);
    beatTree.setProperty ("midiOutDevice", getMidiOutputIdentifier(), nullptr);
    beatTree.setProperty ("editorW",       editorWidth.load(),       nullptr);
    beatTree.setProperty ("editorH",       editorHeight.load(),      nullptr);
    state.addChild (beatTree, -1, nullptr);

    std::unique_ptr<juce::XmlElement> xml (state.createXml());
    copyXmlToBinary (*xml, destData);
}

void PolyrhythmProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, sizeInBytes));
    if (xml == nullptr) return;

    auto state = juce::ValueTree::fromXml (*xml);

    // Projects saved before the duplication fix can hold several beat trees. Each save
    // appended the current one after any stale copies, so the last is the one to trust.
    juce::ValueTree beatTree;
    for (const auto child : state)
        if (child.hasType ("BeatStates"))
            beatTree = child;

    // The beat tree is not parameter state. Keep it out of apvts so it can't linger there
    // and be saved alongside the fresh copy next time.
    auto paramState = state.createCopy();
    for (auto old = paramState.getChildWithName ("BeatStates"); old.isValid();
         old = paramState.getChildWithName ("BeatStates"))
        paramState.removeChild (old, nullptr);
    apvts.replaceState (paramState);

    if (beatTree.isValid())
    {
        for (size_t i = 0; i < MAX_BEATS; ++i)
        {
            trackAActive[i]   = (bool) beatTree.getProperty ("aActive" + juce::String ((int)i), true);
            trackAVelocity[i] = (float)beatTree.getProperty ("aVel"    + juce::String ((int)i), 0.8f);
            trackANotes[i]    = (int)  beatTree.getProperty ("aNote"   + juce::String ((int)i), 36);
            trackACutoff[i]   = (float)beatTree.getProperty ("aCutoff" + juce::String ((int)i), 1.0f);
            trackAGate[i]     = (float)beatTree.getProperty ("aGate"   + juce::String ((int)i), 0.5f);
            trackBActive[i]   = (bool) beatTree.getProperty ("bActive" + juce::String ((int)i), true);
            trackBVelocity[i] = (float)beatTree.getProperty ("bVel"    + juce::String ((int)i), 0.8f);
            trackBNotes[i]    = (int)  beatTree.getProperty ("bNote"   + juce::String ((int)i), 38);
            trackBCutoff[i]   = (float)beatTree.getProperty ("bCutoff" + juce::String ((int)i), 1.0f);
            trackBGate[i]     = (float)beatTree.getProperty ("bGate"   + juce::String ((int)i), 0.5f);
        }

        sendCutoffCC.store ((bool) beatTree.getProperty ("sendCutoffCC", false));
        editorWidth.store  (juce::jmax (EDITOR_BASE_W / 2,
                                        (int) beatTree.getProperty ("editorW", EDITOR_BASE_W)));
        editorHeight.store (juce::jmax (EDITOR_BASE_H / 2,
                                        (int) beatTree.getProperty ("editorH", EDITOR_BASE_H)));

        const juce::String savedDevice = beatTree.getProperty ("midiOutDevice", "").toString();

        if (savedDevice != getMidiOutputIdentifier())
        {
            // Only reopen if that device is actually present on this machine
            const auto devices = getMidiOutputDevices();
            const bool stillAvailable = savedDevice.isNotEmpty()
                && std::any_of (devices.begin(), devices.end(),
                                [&] (const juce::MidiDeviceInfo& d) { return d.identifier == savedDevice; });

            setMidiOutputDevice (stillAvailable ? savedDevice : juce::String());
        }
    }

    uiStateVersion.fetch_add (1);
}

//==============================================================================
// This creates new instances of the plugin
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new PolyrhythmProcessor();
}
