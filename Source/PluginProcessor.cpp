#include "PluginProcessor.h"
#include "PluginEditor.h"

//==============================================================================
PolyrhythmProcessor::PolyrhythmProcessor()
    : AudioProcessor (BusesProperties()
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "Parameters", createParameterLayout())
{
    // Default beat state: only beat 0 active per track
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
}

PolyrhythmProcessor::~PolyrhythmProcessor() {}

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout
PolyrhythmProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    params.push_back (std::make_unique<juce::AudioParameterInt>  ("trackA_beats",   "Track A Beats",          1,  16,  4));
    params.push_back (std::make_unique<juce::AudioParameterInt>  ("trackB_beats",   "Track B Beats",          1,  16,  3));
    params.push_back (std::make_unique<juce::AudioParameterInt>  ("trackA_channel", "Track A MIDI Channel",   1,  16,  1));
    params.push_back (std::make_unique<juce::AudioParameterInt>  ("trackB_channel", "Track B MIDI Channel",   1,  16,  1));
    // Per-beat notes are stored in the beat state ValueTree, not as automatable params
    params.push_back (std::make_unique<juce::AudioParameterFloat>("swing",          "Swing",                  0.0f, 0.5f,  0.0f));
    params.push_back (std::make_unique<juce::AudioParameterFloat>("trackA_gate",    "Track A Gate",           0.01f, 1.0f, 0.5f));
    params.push_back (std::make_unique<juce::AudioParameterFloat>("trackB_gate",    "Track B Gate",           0.01f, 1.0f, 0.5f));
    params.push_back (std::make_unique<juce::AudioParameterFloat>("probability",    "Probability",            0.0f,  1.0f,  1.0f));

    return { params.begin(), params.end() };
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
void PolyrhythmProcessor::prepareToPlay (double sr, int)
{
    sampleRateVal = sr;
    pendingNoteOffs.clear();
}

void PolyrhythmProcessor::releaseResources() {}

bool PolyrhythmProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    if (layouts.getMainInputChannelSet() != juce::AudioChannelSet::disabled())
        return false;
    auto outSet = layouts.getMainOutputChannelSet();
    return outSet == juce::AudioChannelSet::stereo()
        || outSet == juce::AudioChannelSet::mono()
        || outSet == juce::AudioChannelSet::disabled();
}

//==============================================================================
void PolyrhythmProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                         juce::MidiBuffer& midiMessages)
{
    buffer.clear();

    auto* playHead = getPlayHead();
    if (playHead == nullptr) return;

    auto posOpt = playHead->getPosition();
    if (!posOpt.hasValue()) return;

    const bool playing = posOpt->getIsPlaying();
    transportPlaying.store (playing);

    // Update BPM display for the UI
    if (auto bpmOpt = posOpt->getBpm())
        currentBpm.store (*bpmOpt);

    if (!playing)
    {
        if (wasPlaying)
        {
            // Flush all pending note-offs immediately
            for (auto& noff : pendingNoteOffs)
                midiMessages.addEvent (juce::MidiMessage::noteOff (noff.channel, noff.note), 0);
            pendingNoteOffs.clear();
            wasPlaying = false;
        }
        return;
    }

    wasPlaying = true;

    auto ppqOpt = posOpt->getPpqPosition();
    if (!ppqOpt.hasValue()) return;

    const double bpm           = currentBpm.load();
    const double blockStartPpq = *ppqOpt;
    const double ppqPerSample  = bpm / (60.0 * sampleRateVal);
    const double blockEndPpq   = blockStartPpq + buffer.getNumSamples() * ppqPerSample;

    // Bar length in PPQ from the time signature (default 4/4)
    int numerator = 4, denominator = 4;
    if (auto tsOpt = posOpt->getTimeSignature())
    {
        numerator   = tsOpt->numerator;
        denominator = tsOpt->denominator;
    }
    const double barLengthPpq = (4.0 * numerator) / denominator;

    // Process pending note-offs from previous blocks
    {
        std::vector<PendingNoteOff> remaining;
        remaining.reserve (pendingNoteOffs.size());
        for (auto& noff : pendingNoteOffs)
        {
            if (noff.samplesRemaining < buffer.getNumSamples())
                midiMessages.addEvent (juce::MidiMessage::noteOff (noff.channel, noff.note),
                                       noff.samplesRemaining);
            else
                remaining.push_back ({ noff.samplesRemaining - buffer.getNumSamples(),
                                       noff.channel, noff.note });
        }
        pendingNoteOffs = std::move (remaining);
    }

    processTrack (true,  barLengthPpq, blockStartPpq, blockEndPpq, ppqPerSample,
                  buffer.getNumSamples(), midiMessages);
    processTrack (false, barLengthPpq, blockStartPpq, blockEndPpq, ppqPerSample,
                  buffer.getNumSamples(), midiMessages);

    if (audioPreviewEnabled.load())
        renderVoices (buffer);
}

//==============================================================================
void PolyrhythmProcessor::triggerVoice (double frequency, SoundType type, float cutoff)
{
    // Find a free voice, or steal the quietest one
    SynthVoice* target = nullptr;
    float minAmp = 1.0f;
    for (auto& v : voices)
    {
        if (!v.active)  { target = &v; break; }
        if (v.amp < minAmp) { minAmp = v.amp; target = &v; }
    }
    if (target)
    {
        target->phase      = 0.0;
        target->inc        = juce::MathConstants<double>::twoPi * frequency / sampleRateVal;
        target->amp        = 0.5f;
        target->active     = true;
        target->type       = type;
        target->clickCount = (type == SoundType::Click) ? (int)(sampleRateVal * 0.004) : 0;
        target->lpCutoff   = cutoff;
        target->lpState    = 0.0f;
    }
}

void PolyrhythmProcessor::renderVoices (juce::AudioBuffer<float>& buffer)
{
    const int   numSamples  = buffer.getNumSamples();
    const int   numChannels = buffer.getNumChannels();
    // ~80 ms exponential decay (shorter for click/noise)
    const float decayLong  = (float)std::exp (-1.0 / (0.08  * sampleRateVal));
    const float decayShort = (float)std::exp (-1.0 / (0.015 * sampleRateVal));

    for (auto& v : voices)
    {
        if (!v.active) continue;

        double phase = v.phase;
        float  amp   = v.amp;
        const float decay = (v.type == SoundType::Click || v.type == SoundType::Noise)
                            ? decayShort : decayLong;

        // One-pole low-pass: coefficient from cutoff (0-1 maps to 200-20000 Hz)
        const float minF = 200.0f, maxF = 20000.0f;
        const float lpFreq = minF * std::pow (maxF / minF, v.lpCutoff);
        const float lpCoeff = 1.0f - std::exp (-juce::MathConstants<float>::twoPi * lpFreq / (float)sampleRateVal);
        float lpS = v.lpState;

        for (int s = 0; s < numSamples; ++s)
        {
            float sample = 0.0f;
            const double twoPi = juce::MathConstants<double>::twoPi;

            switch (v.type)
            {
                case SoundType::Sine:
                    sample = (float)std::sin (phase);
                    break;

                case SoundType::Triangle:
                    sample = (float)(2.0 / juce::MathConstants<double>::pi * std::asin (std::sin (phase)));
                    break;

                case SoundType::Square:
                    sample = std::sin (phase) >= 0.0 ? 0.8f : -0.8f;
                    break;

                case SoundType::Saw:
                    sample = (float)(std::fmod (phase, twoPi) / juce::MathConstants<double>::pi - 1.0);
                    break;

                case SoundType::Click:
                    if (v.clickCount > 0)
                    {
                        sample = (float)std::sin (phase) * 1.2f;
                        v.clickCount--;
                    }
                    else
                        sample = 0.0f;
                    break;

                case SoundType::Noise:
                    sample = (random.nextFloat() * 2.0f - 1.0f);
                    break;

                default: break;
            }

            sample *= amp;

            // Apply one-pole low-pass filter
            lpS += lpCoeff * (sample - lpS);
            sample = lpS;

            for (int ch = 0; ch < numChannels; ++ch)
                buffer.getWritePointer (ch)[s] += sample;

            phase += v.inc;
            amp   *= decay;
        }
        v.lpState = lpS;

        v.phase = std::fmod (phase, juce::MathConstants<double>::twoPi);
        v.amp   = amp;
        if (v.amp < 0.001f) v.active = false;
    }
}

//==============================================================================
void PolyrhythmProcessor::processTrack (bool isTrackA,
                                         double barLengthPpq,
                                         double blockStartPpq,
                                         double blockEndPpq,
                                         double ppqPerSample,
                                         int    numSamples,
                                         juce::MidiBuffer& midiOut)
{
    const int beatCount = isTrackA ? getTrackABeatCount() : getTrackBBeatCount();
    const int channel   = isTrackA ? getTrackAChannel()   : getTrackBChannel();
    const float prob    = getProbability();
    const float swing   = getSwing();

    auto& activeArr = isTrackA ? trackAActive   : trackBActive;
    auto& velArr    = isTrackA ? trackAVelocity : trackBVelocity;
    auto& noteArr   = isTrackA ? trackANotes    : trackBNotes;

    const double beatInterval = barLengthPpq / std::max (1, beatCount);

    // We check the current bar and one bar ahead to handle block-boundary cases
    const double barStart = std::floor (blockStartPpq / barLengthPpq) * barLengthPpq;

    for (int barOffset = 0; barOffset <= 1; ++barOffset)
    {
        const double currentBarStart = barStart + barOffset * barLengthPpq;

        for (size_t beat = 0; beat < (size_t)beatCount; ++beat)
        {
            if (!activeArr[beat].load()) continue;

            // Apply swing: push odd-numbered beats (1, 3, 5…) forward in time
            double beatOffset = (double)beat * beatInterval;
            if (beat % 2 == 1)
                beatOffset += swing * beatInterval;

            const double beatPpq = currentBarStart + beatOffset;

            if (beatPpq >= blockStartPpq && beatPpq < blockEndPpq)
            {
                // Probability gate
                if (prob < 1.0f && random.nextFloat() > prob) continue;

                int sampleOffset = (int)((beatPpq - blockStartPpq) / ppqPerSample);
                sampleOffset = juce::jlimit (0, numSamples - 1, sampleOffset);

                const int velInt  = juce::jlimit (1, 127,
                    (int)(velArr[beat].load() * 127.0f));
                const int beatNote = juce::jlimit (0, 127, noteArr[beat].load());

                // Send LP cutoff as MIDI CC74 (Brightness) before note-on
                {
                    auto& cutoffArr = isTrackA ? trackACutoff : trackBCutoff;
                    const int ccVal = juce::jlimit (0, 127, (int)(cutoffArr[beat].load() * 127.0f));
                    midiOut.addEvent (juce::MidiMessage::controllerEvent (channel, 74, ccVal),
                                     sampleOffset);
                }

                // Note-on (MIDI)
                midiOut.addEvent (juce::MidiMessage::noteOn (channel, beatNote, (juce::uint8) velInt),
                                  sampleOffset);

                // Signal the UI which beat just fired (for pulse animation)
                if (isTrackA) {
                    trackACurrentBeat.store ((int)beat, std::memory_order_relaxed);
                    trackAFireCount.fetch_add (1, std::memory_order_relaxed);
                } else {
                    trackBCurrentBeat.store ((int)beat, std::memory_order_relaxed);
                    trackBFireCount.fetch_add (1, std::memory_order_relaxed);
                }

                // Audio preview using per-track sound type, note frequency, and cutoff
                if (audioPreviewEnabled.load())
                {
                    auto& cutoffArr = isTrackA ? trackACutoff : trackBCutoff;
                    const double freq = 440.0 * std::pow (2.0, (beatNote - 69) / 12.0);
                    const auto stype = (SoundType)(isTrackA ? trackASoundType.load()
                                                            : trackBSoundType.load());
                    triggerVoice (freq, stype, cutoffArr[beat].load());
                }

                // Per-beat gate
                const float beatGateVal = (isTrackA ? trackAGate[beat] : trackBGate[beat]).load();
                const int gateSamples = std::max (1, (int)(beatGateVal * beatInterval / ppqPerSample));

                // Note-off scheduling
                const int noteOffSample = sampleOffset + gateSamples;
                if (noteOffSample < numSamples)
                    midiOut.addEvent (juce::MidiMessage::noteOff (channel, beatNote), noteOffSample);
                else
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
    beatTree.setProperty ("aSoundType", trackASoundType.load(), nullptr);
    beatTree.setProperty ("bSoundType", trackBSoundType.load(), nullptr);
    beatTree.setProperty ("audioPreview", audioPreviewEnabled.load(), nullptr);
    state.addChild (beatTree, -1, nullptr);

    std::unique_ptr<juce::XmlElement> xml (state.createXml());
    copyXmlToBinary (*xml, destData);
}

void PolyrhythmProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, sizeInBytes));
    if (xml == nullptr) return;

    auto state = juce::ValueTree::fromXml (*xml);
    apvts.replaceState (state);

    auto beatTree = state.getChildWithName ("BeatStates");
    if (beatTree.isValid())
    {
        for (size_t i = 0; i < MAX_BEATS; ++i)
        {
            trackAActive[i]   = (bool) beatTree.getProperty ("aActive" + juce::String ((int)i), i == 0);
            trackAVelocity[i] = (float)beatTree.getProperty ("aVel"    + juce::String ((int)i), 0.8f);
            trackANotes[i]    = (int)  beatTree.getProperty ("aNote"   + juce::String ((int)i), 36);
            trackACutoff[i]   = (float)beatTree.getProperty ("aCutoff" + juce::String ((int)i), 1.0f);
            trackAGate[i]     = (float)beatTree.getProperty ("aGate"   + juce::String ((int)i), 0.5f);
            trackBActive[i]   = (bool) beatTree.getProperty ("bActive" + juce::String ((int)i), i == 0);
            trackBVelocity[i] = (float)beatTree.getProperty ("bVel"    + juce::String ((int)i), 0.8f);
            trackBNotes[i]    = (int)  beatTree.getProperty ("bNote"   + juce::String ((int)i), 38);
            trackBCutoff[i]   = (float)beatTree.getProperty ("bCutoff" + juce::String ((int)i), 1.0f);
            trackBGate[i]     = (float)beatTree.getProperty ("bGate"   + juce::String ((int)i), 0.5f);
        }
        trackASoundType.store ((int)beatTree.getProperty ("aSoundType", (int)SoundType::Sine));
        trackBSoundType.store ((int)beatTree.getProperty ("bSoundType", (int)SoundType::Triangle));
        audioPreviewEnabled.store ((bool)beatTree.getProperty ("audioPreview", true));
    }
}

//==============================================================================
// This creates new instances of the plugin
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new PolyrhythmProcessor();
}
