#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <algorithm>

//==============================================================================
// Parameters the processor (and through uiStateVersion, the editor) reacts to
static constexpr const char* kListenedParams[] = {
    "trackA_beats", "trackB_beats", "trackA_channel", "trackB_channel",
    "trackA_gate",  "trackB_gate",  "trackA_velocity", "trackB_velocity",
    "trackA_lpf",   "trackB_lpf",   "trackA_on",       "trackB_on"
};

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
    for (auto* id : kListenedParams)
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
    for (auto* id : kListenedParams)
        apvts.removeParameterListener (id, this);

    midiOutSender.stopTimer();
    for (int slot = 0; slot < numPortSlots; ++slot)
        setPortDevice (slot, {});
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
    // Track-level velocity and LPF, added after version 1. Like the track gate, each sets
    // every per-beat value on its track.
    params.push_back (std::make_unique<juce::AudioParameterFloat>(PID { "trackA_velocity", 2 }, "Track A Velocity",      0.01f, 1.0f, 0.8f));
    params.push_back (std::make_unique<juce::AudioParameterFloat>(PID { "trackB_velocity", 2 }, "Track B Velocity",      0.01f, 1.0f, 0.8f));
    params.push_back (std::make_unique<juce::AudioParameterFloat>(PID { "trackA_lpf",      2 }, "Track A LPF",           0.0f,  1.0f, 1.0f));
    params.push_back (std::make_unique<juce::AudioParameterFloat>(PID { "trackB_lpf",      2 }, "Track B LPF",           0.0f,  1.0f, 1.0f));

    // Per-rhythm on/off, so a rhythm can be muted or recorded on its own
    params.push_back (std::make_unique<juce::AudioParameterBool> (PID { "trackA_on",       3 }, "Track A On",            true));
    params.push_back (std::make_unique<juce::AudioParameterBool> (PID { "trackB_on",       3 }, "Track B On",            true));

    return { params.begin(), params.end() };
}

//==============================================================================
/*  Can run on the audio thread under automation, so this only stores atomics.

    The track gate, velocity and LPF each set every per-beat value on their track,
    whether they move from the editor or from host automation. On a project load the
    parameters are restored before the beat tree, so saved per-beat values still win.
*/
void PolyrhythmProcessor::parameterChanged (const juce::String& parameterID, float newValue)
{
    auto setAll = [newValue] (std::array<std::atomic<float>, MAX_BEATS>& arr)
    {
        for (auto& v : arr)
            v.store (newValue);
    };

    if      (parameterID == "trackA_gate")     setAll (trackAGate);
    else if (parameterID == "trackB_gate")     setAll (trackBGate);
    else if (parameterID == "trackA_velocity") setAll (trackAVelocity);
    else if (parameterID == "trackB_velocity") setAll (trackBVelocity);
    else if (parameterID == "trackA_lpf")      setAll (trackACutoff);
    else if (parameterID == "trackB_lpf")      setAll (trackBCutoff);

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

void  PolyrhythmProcessor::shiftTrackANotes (int steps)
{
    for (auto& n : trackANotes)
        n.store (stepNote (n.load(), steps));
}
void  PolyrhythmProcessor::shiftTrackBNotes (int steps)
{
    for (auto& n : trackBNotes)
        n.store (stepNote (n.load(), steps));
}
void  PolyrhythmProcessor::resetTrackANotes()
{
    for (auto& n : trackANotes)
        n.store (snapToScale (36));   // C2
}
void  PolyrhythmProcessor::resetTrackBNotes()
{
    for (auto& n : trackBNotes)
        n.store (snapToScale (38));   // D2
}

//==============================================================================
// Scale lock. Keep the order and steps in step with SCALES in docs/index.html.
const std::array<PolyrhythmProcessor::Scale, 12>& PolyrhythmProcessor::getScales()
{
    static const std::array<Scale, 12> scales {{
        { "Major (Ionian)",   7, { 0, 2, 4, 5, 7, 9, 11 } },
        { "Minor (Aeolian)",  7, { 0, 2, 3, 5, 7, 8, 10 } },
        { "Dorian",           7, { 0, 2, 3, 5, 7, 9, 10 } },
        { "Phrygian",         7, { 0, 1, 3, 5, 7, 8, 10 } },
        { "Lydian",           7, { 0, 2, 4, 6, 7, 9, 11 } },
        { "Mixolydian",       7, { 0, 2, 4, 5, 7, 9, 10 } },
        { "Locrian",          7, { 0, 1, 3, 5, 6, 8, 10 } },
        { "Harmonic minor",   7, { 0, 2, 3, 5, 7, 8, 11 } },
        { "Melodic minor",    7, { 0, 2, 3, 5, 7, 9, 11 } },
        { "Major pentatonic", 5, { 0, 2, 4, 7, 9 } },
        { "Minor pentatonic", 5, { 0, 3, 5, 7, 10 } },
        { "Blues",            6, { 0, 3, 5, 6, 7, 10 } },
    }};
    return scales;
}

bool PolyrhythmProcessor::isInScale (int note) const
{
    if (! scaleEnabled.load())
        return true;

    const auto& scale  = getScales()[(size_t) juce::jlimit (0, 11, scaleType.load())];
    const int   degree = ((note - scaleRoot.load()) % 12 + 12) % 12;

    for (int i = 0; i < scale.numSteps; ++i)
        if (scale.steps[i] == degree)
            return true;

    return false;
}

int PolyrhythmProcessor::snapToScale (int note) const
{
    for (int d = 0; d < 12; ++d)
    {
        if (note - d >= 0   && isInScale (note - d)) return note - d;
        if (note + d <= 127 && isInScale (note + d)) return note + d;
    }
    return note;
}

int PolyrhythmProcessor::stepNote (int note, int delta) const
{
    if (delta == 0)
        return note;

    // Repeat for multi-step deltas, one degree at a time
    const int dir = delta > 0 ? 1 : -1;
    for (int s = 0; s < std::abs (delta); ++s)
    {
        int m = note;
        do { m += dir; } while (m >= 0 && m <= 127 && ! isInScale (m));

        if (m < 0 || m > 127)
            break;   // no further scale note in this direction
        note = m;
    }
    return note;
}

void PolyrhythmProcessor::randomiseTrack (bool isTrackA, RandomTarget what)
{
    auto& notes  = isTrackA ? trackANotes    : trackBNotes;
    auto& vels   = isTrackA ? trackAVelocity : trackBVelocity;
    auto& gates  = isTrackA ? trackAGate     : trackBGate;
    auto& cuts   = isTrackA ? trackACutoff   : trackBCutoff;
    const int count = juce::jlimit (1, MAX_BEATS, isTrackA ? getTrackABeatCount()
                                                           : getTrackBBeatCount());
    const bool all = what == RandomTarget::all;

    // Separate from the audio thread's generator, which drives probability
    juce::Random rng;
    auto range = [&rng] (float lo, float hi) { return lo + rng.nextFloat() * (hi - lo); };

    if (all || what == RandomTarget::notes)
    {
        // Within an octave centred on the current notes, so repeated presses wander rather
        // than jump registers. In key when the scale lock is on.
        int sum = 0;
        for (int i = 0; i < count; ++i)
            sum += notes[(size_t) i].load();
        const int centre = (int) std::lround ((double) sum / count);

        for (int i = 0; i < count; ++i)
            notes[(size_t) i].store (snapToScale (juce::jlimit (0, 127, centre - 6 + rng.nextInt (13))));
    }

    // Lower limits keep a beat from going silent or fully muffled. Same ranges as the web.
    for (int i = 0; i < count; ++i)
    {
        if (all || what == RandomTarget::velocity) vels [(size_t) i].store (range (0.35f, 1.0f));
        if (all || what == RandomTarget::gate)     gates[(size_t) i].store (range (0.1f,  1.0f));
        if (all || what == RandomTarget::cutoff)   cuts [(size_t) i].store (range (0.2f,  1.0f));
    }

    markBeatStateEdited();
    uiStateVersion.fetch_add (1);
}

void PolyrhythmProcessor::snapAllNotesToScale()
{
    if (! scaleEnabled.load())
        return;

    for (auto* arr : { &trackANotes, &trackBNotes })
        for (auto& n : *arr)
            n.store (snapToScale (n.load()));

    markBeatStateEdited();
}
float PolyrhythmProcessor::getSwing()           const { return *apvts.getRawParameterValue ("swing"); }
float PolyrhythmProcessor::getTrackAGate()      const { return *apvts.getRawParameterValue ("trackA_gate"); }
float PolyrhythmProcessor::getTrackBGate()      const { return *apvts.getRawParameterValue ("trackB_gate"); }
float PolyrhythmProcessor::getTrackAVelocity()  const { return *apvts.getRawParameterValue ("trackA_velocity"); }
float PolyrhythmProcessor::getTrackBVelocity()  const { return *apvts.getRawParameterValue ("trackB_velocity"); }
float PolyrhythmProcessor::getTrackACutoff()    const { return *apvts.getRawParameterValue ("trackA_lpf"); }
float PolyrhythmProcessor::getTrackBCutoff()    const { return *apvts.getRawParameterValue ("trackB_lpf"); }
float PolyrhythmProcessor::getProbability()     const { return *apvts.getRawParameterValue ("probability"); }

//==============================================================================
// Direct MIDI output devices
juce::Array<juce::MidiDeviceInfo> PolyrhythmProcessor::getMidiOutputDevices()
{
    return juce::MidiOutput::getAvailableDevices();
}

void PolyrhythmProcessor::setPortDevice (int slot, const juce::String& identifier)
{
    auto& port = outputPorts[(size_t) slot];

    std::unique_ptr<juce::MidiOutput> newDevice;

    if (identifier.isNotEmpty())
        newDevice = juce::MidiOutput::openDevice (identifier);

    std::unique_ptr<juce::MidiOutput> previous;
    {
        const juce::ScopedLock sl (midiOutLock);
        previous        = std::move (port.device);
        port.device     = std::move (newDevice);
        port.identifier = (port.device != nullptr) ? identifier : juce::String();
        port.active.store (port.device != nullptr, std::memory_order_relaxed);
    }

    // Notes started on the old port would never get their note-off once the routing moves
    // elsewhere, so close them before letting it go
    if (previous != nullptr)
        for (const int channel : { getTrackAChannel(), getTrackBChannel() })
            previous->sendMessageNow (juce::MidiMessage::allNotesOff (channel));

    const bool anyActive = std::any_of (outputPorts.begin(), outputPorts.end(),
                                        [] (const OutputPort& p) { return p.active.load(); });

    if (anyActive)
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

juce::String PolyrhythmProcessor::getPortIdentifier (int slot) const
{
    const juce::ScopedLock sl (midiOutLock);
    return outputPorts[(size_t) slot].identifier;
}

juce::String PolyrhythmProcessor::getPortName (int slot) const
{
    const juce::ScopedLock sl (midiOutLock);
    const auto& device = outputPorts[(size_t) slot].device;
    return device != nullptr ? device->getName() : juce::String();
}

// Reopens a port from a saved project, but only if that device exists on this machine
void PolyrhythmProcessor::restorePortDevice (int slot, const juce::String& savedIdentifier)
{
    if (savedIdentifier == getPortIdentifier (slot))
        return;

    const auto devices = getMidiOutputDevices();
    const bool stillAvailable = savedIdentifier.isNotEmpty()
        && std::any_of (devices.begin(), devices.end(),
                        [&] (const juce::MidiDeviceInfo& d) { return d.identifier == savedIdentifier; });

    setPortDevice (slot, stillAvailable ? savedIdentifier : juce::String());
}

int PolyrhythmProcessor::destinationSlot (int track) const
{
    const int own = track == 0 ? trackAPort : trackBPort;

    if (outputPorts[(size_t) own].active.load (std::memory_order_relaxed))
        return own;

    if (outputPorts[mainPort].active.load (std::memory_order_relaxed))
        return mainPort;

    return -1;
}

void PolyrhythmProcessor::setMidiOutputDevice (const juce::String& identifier)   { setPortDevice (mainPort, identifier); }
juce::String PolyrhythmProcessor::getMidiOutputIdentifier() const                { return getPortIdentifier (mainPort); }
juce::String PolyrhythmProcessor::getMidiOutputName() const                      { return getPortName (mainPort); }

void PolyrhythmProcessor::setTrackMidiOutputDevice (bool isTrackA, const juce::String& identifier)
{
    setPortDevice (isTrackA ? trackAPort : trackBPort, identifier);
}

juce::String PolyrhythmProcessor::getTrackMidiOutputIdentifier (bool isTrackA) const
{
    return getPortIdentifier (isTrackA ? trackAPort : trackBPort);
}

juce::String PolyrhythmProcessor::getTrackMidiOutputName (bool isTrackA) const
{
    return getPortName (isTrackA ? trackAPort : trackBPort);
}

bool PolyrhythmProcessor::isTrackOn (bool isTrackA) const
{
    return *apvts.getRawParameterValue (isTrackA ? "trackA_on" : "trackB_on") >= 0.5f;
}

//==============================================================================
/*  Queues both rhythms' messages for the sender thread, each tagged with its rhythm's
    port. The two buffers are merged by sample position: the sender stops at the first
    message that is not due yet, so the queue has to stay in time order.
*/
void PolyrhythmProcessor::queueForExternalMidiOut (int numSamples)
{
    const int dest[2] = { destinationSlot (0), destinationSlot (1) };

    if (dest[0] < 0 && dest[1] < 0)
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

    auto push = [&] (const juce::MidiMessageMetadata& meta, int port)
    {
        if (port < 0 || meta.numBytes < 1 || meta.numBytes > 3)
            return true;

        int start1, size1, start2, size2;
        midiOutFifo.prepareToWrite (1, start1, size1, start2, size2);

        // Full queue: drop rather than block the audio thread
        if (size1 + size2 < 1)
            return false;

        auto& slot = midiOutQueue[(size_t) (size1 > 0 ? start1 : start2)];
        std::memcpy (slot.bytes, meta.data, (size_t) meta.numBytes);
        slot.size     = (juce::uint8) meta.numBytes;
        slot.port     = (juce::uint8) port;
        slot.sendAtMs = blockStartMs + meta.samplePosition * msPerSmp;

        midiOutFifo.finishedWrite (1);
        return true;
    };

    auto itA = generatedMidi[0].begin(), endA = generatedMidi[0].end();
    auto itB = generatedMidi[1].begin(), endB = generatedMidi[1].end();

    while (itA != endA || itB != endB)
    {
        const bool takeA = itB == endB
                        || (itA != endA && (*itA).samplePosition <= (*itB).samplePosition);

        if (! (takeA ? push (*itA++, dest[0]) : push (*itB++, dest[1])))
            return;
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

                // A port closed since this was queued just drops the message
                auto& device = outputPorts[(size_t) slot.port].device;

                if (device != nullptr && slot.size > 0)
                    device->sendMessageNow (juce::MidiMessage (slot.bytes, (int) slot.size));

                ++sent;
            }

            return true;
        };

        if (sendRange (start1, size1))
            sendRange (start2, size2);
    }

    midiOutFifo.finishedRead (sent);
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

void PolyrhythmProcessor::flushPendingNoteOffs (int sampleOffset)
{
    for (auto& noff : pendingNoteOffs)
        generatedMidi[(size_t) noff.track].addEvent (juce::MidiMessage::noteOff (noff.channel, noff.note),
                                                     sampleOffset);

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
    for (auto& buffer : generatedMidi)
        buffer.clear();

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
            flushPendingNoteOffs (0);
            generatedMidi[0].addEvent (juce::MidiMessage::allNotesOff (getTrackAChannel()), 0);
            generatedMidi[1].addEvent (juce::MidiMessage::allNotesOff (getTrackBChannel()), 0);

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
        flushPendingNoteOffs (0);
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
                generatedMidi[(size_t) noff.track].addEvent (juce::MidiMessage::noteOff (noff.channel, noff.note),
                                        noff.samplesRemaining);
            }
            else
            {
                pendingNoteOffs[keep++] = { noff.samplesRemaining - numSamples,
                                            noff.channel, noff.note, noff.track };
            }
        }
        pendingNoteOffs.resize (keep);
    }

    if (numSamples > 0 && blockEndPpq > windowStartPpq)
    {
        processTrack (true,  barLengthPpq, barAnchorPpq, windowStartPpq, blockEndPpq,
                      blockStartPpq, ppqPerSample, numSamples, generatedMidi[0]);
        processTrack (false, barLengthPpq, barAnchorPpq, windowStartPpq, blockEndPpq,
                      blockStartPpq, ppqPerSample, numSamples, generatedMidi[1]);
    }

    nextPpq = juce::jmax (nextPpq, blockEndPpq);

    publishGeneratedMidi (midiMessages, numSamples);
}

//==============================================================================
/*  Generated notes go two places: into the host's buffer, where they join the
    pass-through input on the way to the instrument, and onto each rhythm's external
    port. Only the generated half reaches a port; see queueForExternalMidiOut.
*/
void PolyrhythmProcessor::publishGeneratedMidi (juce::MidiBuffer& hostBuffer, int numSamples)
{
    queueForExternalMidiOut (numSamples);

    for (const auto& buffer : generatedMidi)
        if (! buffer.isEmpty())
            hostBuffer.addEvents (buffer, 0, numSamples > 0 ? numSamples : -1, 0);
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
    // A rhythm that is switched off starts no new notes. Its note-offs still go out,
    // through the pending list, so switching off mid-note leaves nothing hanging.
    if (! isTrackOn (isTrackA))
        return;

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
                pendingNoteOffs.push_back ({ noteOffSample - numSamples, channel, beatNote,
                                             isTrackA ? 0 : 1 });
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
    beatTree.setProperty ("scaleOn",       scaleEnabled.load(),      nullptr);
    beatTree.setProperty ("scaleRoot",     scaleRoot.load(),         nullptr);
    beatTree.setProperty ("scaleType",     scaleType.load(),         nullptr);
    beatTree.setProperty ("midiOutDevice", getMidiOutputIdentifier(), nullptr);
    beatTree.setProperty ("midiOutDeviceA", getTrackMidiOutputIdentifier (true),  nullptr);
    beatTree.setProperty ("midiOutDeviceB", getTrackMidiOutputIdentifier (false), nullptr);
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
        scaleEnabled.store ((bool) beatTree.getProperty ("scaleOn", false));
        scaleRoot.store    (juce::jlimit (0, 11, (int) beatTree.getProperty ("scaleRoot", 0)));
        scaleType.store    (juce::jlimit (0, (int) getScales().size() - 1,
                                          (int) beatTree.getProperty ("scaleType", 0)));
        editorWidth.store  (juce::jmax (EDITOR_BASE_W / 2,
                                        (int) beatTree.getProperty ("editorW", EDITOR_BASE_W)));
        editorHeight.store (juce::jmax (EDITOR_BASE_H / 2,
                                        (int) beatTree.getProperty ("editorH", EDITOR_BASE_H)));

        restorePortDevice (mainPort,   beatTree.getProperty ("midiOutDevice",  "").toString());
        restorePortDevice (trackAPort, beatTree.getProperty ("midiOutDeviceA", "").toString());
        restorePortDevice (trackBPort, beatTree.getProperty ("midiOutDeviceB", "").toString());
    }

    uiStateVersion.fetch_add (1);
}

//==============================================================================
// This creates new instances of the plugin
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new PolyrhythmProcessor();
}
