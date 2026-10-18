/*
    Drives the real PolyrhythmProcessor with a fake playhead and checks that the beat
    scheduler emits exactly the notes it should.

    The bug this exists to catch: the scheduler used to search [blockStartPpq, blockEndPpq)
    with blockEndPpq derived from bpm and sample rate. Host-reported block positions do not
    tile that window exactly, so beats fell into the gaps between blocks (dropped notes) or
    into overlaps (doubled notes). Cases 2 and 3 below reproduce both.

    Build and run:
        cmake --build build --target SchedulerTest && ./build/SchedulerTest_artefacts/SchedulerTest
*/

#ifdef __APPLE__
 #include <CoreFoundation/CoreFoundation.h>   // before JUCE, whose Point clashes with MacTypes
#endif
#include <JuceHeader.h>
#include "../Source/PluginProcessor.h"
#include <cstdio>
#include <map>
#include <vector>

//==============================================================================
struct FakePlayHead : juce::AudioPlayHead
{
    double ppq       = 0.0;
    double bpm       = 120.0;
    bool   playing   = true;
    int    tsNum     = 4;
    int    tsDen     = 4;

    // PPQ of any bar line. Zero unless the bar grid is offset from the timeline, as it is
    // after a pickup bar or an earlier time signature change.
    double barAnchor = 0.0;

    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setIsPlaying (playing);
        info.setPpqPosition (ppq);
        info.setBpm (bpm);
        info.setTimeSignature (TimeSignature { tsNum, tsDen });

        const double barLength = 4.0 * tsNum / tsDen;
        info.setPpqPositionOfLastBarStart (barAnchor
                                           + std::floor ((ppq - barAnchor) / barLength) * barLength);
        return info;
    }

    bool canControlTransport() override { return false; }
};

//==============================================================================
struct Capture
{
    int noteOns  = 0;
    int noteOffs = 0;
    int ccEvents = 0;
    std::map<int, int> onsPerChannel;     // channel -> count
    std::vector<double> onPpqs;           // absolute PPQ of every note-on

    // Every note event in output order, for checks that care about what is held when
    struct NoteEvent { bool on; int channel; int note; double ppq; };
    std::vector<NoteEvent> noteEvents;

    void ingest (const juce::MidiBuffer& buf, double blockStartPpq, double ppqPerSample)
    {
        for (const auto meta : buf)
        {
            const auto m = meta.getMessage();

            const double ppq = blockStartPpq + meta.samplePosition * ppqPerSample;

            if (m.isNoteOn())
            {
                ++noteOns;
                ++onsPerChannel[m.getChannel()];
                onPpqs.push_back (ppq);
                noteEvents.push_back ({ true, m.getChannel(), m.getNoteNumber(), ppq });
            }
            else if (m.isNoteOff())
            {
                ++noteOffs;
                noteEvents.push_back ({ false, m.getChannel(), m.getNoteNumber(), ppq });
            }
            else if (m.isController())   ++ccEvents;
        }
    }
};

//==============================================================================
static int failures = 0;

static void check (bool condition, const juce::String& what)
{
    std::printf ("  [%s] %s\n", condition ? "PASS" : "FAIL", what.toRawUTF8());
    if (! condition) ++failures;
}

//==============================================================================
/*  Runs the transport forward by `targetPpq` quarter notes.

    The true position always advances by exactly one block. `jitterPpq` is added to the
    position the host *reports*, alternating sign block to block, which is what real hosts
    do — reported positions wobble around the true one rather than drifting away from it.
    That wobble is what opens gaps (beat falls between two windows and is lost) and
    overlaps (beat falls in both windows and fires twice).
*/
static Capture runTransport (PolyrhythmProcessor& proc,
                             FakePlayHead& playHead,
                             double targetPpq,
                             int    blockSize,
                             double sampleRate,
                             double jitterPpq = 0.0,
                             juce::Random* rng = nullptr)
{
    Capture cap;

    const double ppqPerSample = playHead.bpm / (60.0 * sampleRate);
    const double blockPpq     = blockSize * ppqPerSample;
    const int    numBlocks    = (int) std::floor (targetPpq / blockPpq);

    juce::AudioBuffer<float> audio (0, blockSize);
    double truePpq = playHead.ppq;

    for (int i = 0; i < numBlocks; ++i)
    {
        // Wobble the reported position around the true one. Random offsets matter here:
        // a fixed alternating offset produces gaps at fixed places, which the beat grid
        // can miss entirely by luck.
        double jitter = 0.0;

        if (i > 0 && jitterPpq > 0.0)
            jitter = rng != nullptr ? (rng->nextDouble() * 2.0 - 1.0) * jitterPpq
                                    : ((i % 2) ? jitterPpq : -jitterPpq);

        playHead.ppq = truePpq + jitter;

        juce::MidiBuffer midi;
        const double blockStart = playHead.ppq;

        proc.processBlock (audio, midi);
        cap.ingest (midi, blockStart, ppqPerSample);

        truePpq += blockPpq;
    }

    playHead.ppq = truePpq;
    return cap;
}

//==============================================================================
static void expectBeatsPerBar (const Capture& cap, int bars, int beatsA, int beatsB,
                               const juce::String& label)
{
    const int expected = bars * (beatsA + beatsB);

    check (cap.noteOns == expected,
           label + ": expected " + juce::String (expected) + " note-ons, got "
                 + juce::String (cap.noteOns));

    check (cap.noteOffs == cap.noteOns,
           label + ": note-offs balance note-ons (" + juce::String (cap.noteOffs)
                 + " vs " + juce::String (cap.noteOns) + ")");

    // No duplicate note-ons at the same position — a doubled beat shows up here
    int duplicates = 0;
    for (size_t i = 1; i < cap.onPpqs.size(); ++i)
        for (size_t j = 0; j < i; ++j)
            if (std::abs (cap.onPpqs[i] - cap.onPpqs[j]) < 1.0e-6)
                ++duplicates;

    // Tracks A and B legitimately coincide on the downbeat of every bar
    check (duplicates <= bars,
           label + ": no unexpected duplicate note-ons (" + juce::String (duplicates)
                 + " coincident, " + juce::String (bars) + " allowed for shared downbeats)");
}

static void setParam (PolyrhythmProcessor& proc, const juce::String& id, float value)
{
    if (auto* p = proc.apvts.getParameter (id))
        p->setValueNotifyingHost (p->convertTo0to1 (value));
}

static int countBeatStateTrees (const juce::MemoryBlock& block)
{
    auto xml = juce::AudioProcessor::getXmlFromBinary (block.getData(), (int) block.getSize());
    if (xml == nullptr)
        return -1;

    int count = 0;
    for (auto* child : xml->getChildIterator())
        if (child->hasTagName ("BeatStates"))
            ++count;

    return count;
}

//==============================================================================
int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    constexpr double sampleRate = 48000.0;
    constexpr int    blockSize  = 512;
    constexpr double bpm        = 120.0;

    const double ppqPerSample = bpm / (60.0 * sampleRate);
    const double blockPpq     = blockSize * ppqPerSample;

    /*  Beat positions in PPQ for the defaults (4/4, track A = 4 beats, track B = 3):

            bar 1                     bar 2
            A: 0    1    2    3       A: 4    5    6    7
            B: 0    1.33 2.67         B: 4    5.33 6.67

        Stopping anywhere in (7.0, 8.0) captures exactly two full bars without touching
        bar 3's downbeat, so 7.6 leaves plenty of room either side for jitter.
    */
    constexpr double twoBars = 7.6;
    constexpr double oneBar  = 3.6;   // likewise, anywhere in (3.0, 4.0)

    // Jitter of 2% of a block is far below the discontinuity threshold, so it reads as
    // normal forward motion rather than a relocate
    const double jitter = blockPpq * 0.02;

    auto makeProcessor = [&] (std::unique_ptr<PolyrhythmProcessor>& p, FakePlayHead& ph)
    {
        p = std::make_unique<PolyrhythmProcessor>();
        p->setPlayHead (&ph);
        p->prepareToPlay (sampleRate, blockSize);
    };

    //--------------------------------------------------------------------------
    std::printf ("\nCase 1 — ideal contiguous transport\n");
    {
        FakePlayHead ph; ph.bpm = bpm;
        std::unique_ptr<PolyrhythmProcessor> proc;
        makeProcessor (proc, ph);

        auto cap = runTransport (*proc, ph, twoBars, blockSize, sampleRate);
        expectBeatsPerBar (cap, 2, 4, 3, "ideal");

        check (cap.ccEvents == 0, "ideal: no CC74 emitted by default");
        check (cap.onsPerChannel[1] == 2 * 4, "ideal: track A on MIDI channel 1");
        check (cap.onsPerChannel[2] == 2 * 3, "ideal: track B on MIDI channel 2");
    }

    //--------------------------------------------------------------------------
    /*  The important case. A single fixed jitter pattern is not enough: the gaps land at
        fixed positions and the beat grid can sail straight past them. Sweeping many seeds
        puts a gap under a beat sooner or later, which is exactly what happens in a real
        session over minutes of playback.
    */
    std::printf ("\nCase 2 — jittery host positions, swept over 400 seeds\n");
    {
        for (const double amount : { jitter, blockPpq / 3.0, blockPpq * 0.9 })
        {
            int badRuns = 0, worstCount = 14;

            for (int seed = 0; seed < 400; ++seed)
            {
                FakePlayHead ph; ph.bpm = bpm;
                std::unique_ptr<PolyrhythmProcessor> proc;
                makeProcessor (proc, ph);

                juce::Random rng (seed);
                auto cap = runTransport (*proc, ph, twoBars, blockSize, sampleRate,
                                         amount, &rng);

                if (cap.noteOns != 14 || cap.noteOffs != cap.noteOns)
                {
                    ++badRuns;
                    if (std::abs (cap.noteOns - 14) > std::abs (worstCount - 14))
                        worstCount = cap.noteOns;
                }
            }

            check (badRuns == 0,
                   "jitter " + juce::String (amount / blockPpq, 2)
                       + " blocks: 400/400 runs emitted exactly 14 notes ("
                       + juce::String (badRuns) + " bad, worst count "
                       + juce::String (worstCount) + ")");
        }
    }

    //--------------------------------------------------------------------------
    std::printf ("\nCase 3 — loop back to the top of the bar\n");
    {
        FakePlayHead ph; ph.bpm = bpm;
        std::unique_ptr<PolyrhythmProcessor> proc;
        makeProcessor (proc, ph);

        auto first = runTransport (*proc, ph, oneBar, blockSize, sampleRate);

        // Jump the transport back to zero, as a loop does
        ph.ppq = 0.0;
        auto second = runTransport (*proc, ph, oneBar, blockSize, sampleRate);

        check (first.noteOns == 7,
               "loop: 7 note-ons before the loop, got " + juce::String (first.noteOns));
        check (second.noteOns == 7,
               "loop: sequencer restarts after the jump, got " + juce::String (second.noteOns));
        check (first.noteOffs + second.noteOffs == first.noteOns + second.noteOns,
               "loop: every note is closed, no hanging notes across the jump");
    }

    //--------------------------------------------------------------------------
    std::printf ("\nCase 4 — transport stop closes every note\n");
    {
        FakePlayHead ph; ph.bpm = bpm;
        std::unique_ptr<PolyrhythmProcessor> proc;
        makeProcessor (proc, ph);

        auto running = runTransport (*proc, ph, 2.0, blockSize, sampleRate);

        ph.playing = false;
        auto stopped = runTransport (*proc, ph, blockPpq * 2.0, blockSize, sampleRate);

        check (running.noteOffs + stopped.noteOffs >= running.noteOns,
               "stop: all outstanding notes closed on transport stop");
    }

    //--------------------------------------------------------------------------
    std::printf ("\nCase 5 — CC74 only emitted on change when enabled\n");
    {
        FakePlayHead ph; ph.bpm = bpm;
        std::unique_ptr<PolyrhythmProcessor> proc;
        makeProcessor (proc, ph);
        proc->sendCutoffCC.store (true);

        auto cap = runTransport (*proc, ph, twoBars, blockSize, sampleRate);

        // Every beat has the same default cutoff, so one CC per channel is enough
        check (cap.ccEvents == 2,
               "cc74: one CC per channel for a constant cutoff, got "
                   + juce::String (cap.ccEvents) + " (was one per note before)");
    }

    //--------------------------------------------------------------------------
    /*  Input has to reach the instrument downstream untouched. The external-port feed is
        deliberately fed only the generated half — echoing input there loops the moment the
        DAW listens to the same port — so this guards against fixing that by dropping input.
    */
    std::printf ("\nCase 6 — host input passes through to the output buffer\n");
    {
        FakePlayHead ph; ph.bpm = bpm;
        std::unique_ptr<PolyrhythmProcessor> proc;
        makeProcessor (proc, ph);

        juce::AudioBuffer<float> audio (0, blockSize);
        juce::MidiBuffer midi;

        // A note the host is feeding in, on a channel we never generate on
        midi.addEvent (juce::MidiMessage::noteOn (10, 60, (juce::uint8) 100), 0);
        proc->processBlock (audio, midi);

        bool foundInput = false, foundGenerated = false;

        for (const auto meta : midi)
        {
            const auto m = meta.getMessage();
            if (m.isNoteOn() && m.getChannel() == 10 && m.getNoteNumber() == 60) foundInput = true;
            if (m.isNoteOn() && m.getChannel() == 1)                             foundGenerated = true;
        }

        check (foundInput,     "passthrough: the host's own note survives to the output");
        check (foundGenerated, "passthrough: our generated note is in the same buffer");
    }

    //--------------------------------------------------------------------------
    /*  Swing delays odd beats, which shortens the gap to the beat after. A gate measured
        against the plain beat interval then runs past the next note-on of the same pitch,
        and the late note-off cuts that next note short on the instrument.
    */
    std::printf ("\nCase 7 — swing with a long gate never overlaps the same note\n");
    {
        FakePlayHead ph; ph.bpm = bpm;
        std::unique_ptr<PolyrhythmProcessor> proc;
        makeProcessor (proc, ph);

        setParam (*proc, "swing", 0.3f);
        for (size_t i = 0; i < MAX_BEATS; ++i)
        {
            proc->trackAGate[i].store (0.95f);
            proc->trackBGate[i].store (0.95f);
        }

        auto cap = runTransport (*proc, ph, twoBars, blockSize, sampleRate);

        std::map<std::pair<int, int>, bool> held;
        int overlaps = 0;

        for (const auto& e : cap.noteEvents)
        {
            auto& isHeld = held[{ e.channel, e.note }];

            if (e.on && isHeld)
                ++overlaps;

            isHeld = e.on;
        }

        check (cap.noteOns == 14,
               "swing: still 14 note-ons over two bars, got " + juce::String (cap.noteOns));
        check (overlaps == 0,
               "swing: no note-on while the same note is still held ("
                   + juce::String (overlaps) + " overlaps)");
    }

    //--------------------------------------------------------------------------
    /*  Bars are not always multiples of the bar length from PPQ zero. After a pickup bar or
        an earlier time signature change the host's bar lines sit elsewhere, and beat 1 has
        to follow them rather than an imagined grid starting at zero.
    */
    std::printf ("\nCase 8 — downbeats follow the host's bar lines\n");
    {
        FakePlayHead ph; ph.bpm = bpm;
        ph.tsNum = 3; ph.tsDen = 4;
        ph.barAnchor = 1.0;   // bars start at 1, 4, 7...

        std::unique_ptr<PolyrhythmProcessor> proc;
        makeProcessor (proc, ph);
        setParam (*proc, "trackA_beats", 1.0f);

        auto cap = runTransport (*proc, ph, twoBars, blockSize, sampleRate);

        std::vector<double> downbeats;
        for (const auto& e : cap.noteEvents)
            if (e.on && e.channel == 1)
                downbeats.push_back (e.ppq);

        bool allOnBarLines = ! downbeats.empty();
        for (const double ppq : downbeats)
        {
            const double intoBar = std::fmod (ppq - ph.barAnchor + 3.0, 3.0);
            if (std::min (intoBar, 3.0 - intoBar) > 2.0 * ppqPerSample)
                allOnBarLines = false;
        }

        check (downbeats.size() == 3,
               "bars: one downbeat per bar at 1, 4 and 7, got "
                   + juce::String ((int) downbeats.size()));
        check (allOnBarLines, "bars: every track A note lands on a host bar line");
    }

    //--------------------------------------------------------------------------
    /*  Loading state used to keep the saved beat tree inside the parameter state, so the
        next save appended a second copy after it. The loader reads the first copy, so any
        beat edit made after reopening a project was lost on the following reopen.
    */
    std::printf ("\nCase 9 — beat edits survive a second save and load\n");
    {
        PolyrhythmProcessor first;
        first.trackANotes[2].store (60);

        juce::MemoryBlock saved;
        first.getStateInformation (saved);

        PolyrhythmProcessor second;
        second.setStateInformation (saved.getData(), (int) saved.getSize());
        second.trackANotes[2].store (62);

        juce::MemoryBlock resaved;
        second.getStateInformation (resaved);

        PolyrhythmProcessor third;
        third.setStateInformation (resaved.getData(), (int) resaved.getSize());

        check (third.trackANotes[2].load() == 62,
               "state: edit after a reload is restored, got note "
                   + juce::String (third.trackANotes[2].load()));
        check (countBeatStateTrees (resaved) == 1,
               "state: exactly one beat tree after a round trip, got "
                   + juce::String (countBeatStateTrees (resaved)));
    }

    //--------------------------------------------------------------------------
    // Projects saved by the old code carry a stale beat tree first and the real one last
    std::printf ("\nCase 10 — projects saved with duplicate beat trees load the newest\n");
    {
        PolyrhythmProcessor source;
        source.trackANotes[2].store (62);

        juce::MemoryBlock saved;
        source.getStateInformation (saved);

        auto xml = juce::AudioProcessor::getXmlFromBinary (saved.getData(), (int) saved.getSize());
        auto* fresh = xml->getChildByName ("BeatStates");

        auto* stale = new juce::XmlElement (*fresh);
        stale->setAttribute ("aNote2", 60);
        xml->insertChildElement (stale, 0);

        juce::MemoryBlock legacy;
        juce::AudioProcessor::copyXmlToBinary (*xml, legacy);

        PolyrhythmProcessor loaded;
        loaded.setStateInformation (legacy.getData(), (int) legacy.getSize());

        juce::MemoryBlock resaved;
        loaded.getStateInformation (resaved);

        check (loaded.trackANotes[2].load() == 62,
               "legacy: the last beat tree wins, got note "
                   + juce::String (loaded.trackANotes[2].load()));
        check (countBeatStateTrees (resaved) == 1,
               "legacy: the next save drops the stale copies, got "
                   + juce::String (countBeatStateTrees (resaved)));
    }

    //--------------------------------------------------------------------------
    // Hosts automate the track gate parameter. It used to move only when dragged in the UI.
    std::printf ("\nCase 11 — automating the track gate sets every beat gate\n");
    {
        PolyrhythmProcessor proc;
        setParam (proc, "trackA_gate", 0.8f);

        bool allSet = true;
        for (const auto& g : proc.trackAGate)
            allSet = allSet && std::abs (g.load() - 0.8f) < 1.0e-4f;

        check (allSet, "gate: every track A beat gate follows the parameter");
        check (std::abs (proc.trackBGate[0].load() - 0.5f) < 1.0e-4f,
               "gate: track B is left alone");
    }

    //--------------------------------------------------------------------------
    // Loading restores the track gate parameter, which must not flatten saved beat gates
    std::printf ("\nCase 12 — loading keeps per-beat gates and tells the editor\n");
    {
        PolyrhythmProcessor source;
        setParam (source, "trackA_gate", 0.7f);
        source.trackAGate[1].store (0.2f);

        juce::MemoryBlock saved;
        source.getStateInformation (saved);

        PolyrhythmProcessor loaded;
        const auto versionBefore = loaded.uiStateVersion.load();
        loaded.setStateInformation (saved.getData(), (int) saved.getSize());

        check (std::abs (loaded.trackAGate[1].load() - 0.2f) < 1.0e-4f,
               "load: an edited beat gate survives, got "
                   + juce::String (loaded.trackAGate[1].load()));
        check (std::abs (loaded.trackAGate[0].load() - 0.7f) < 1.0e-4f,
               "load: other beats keep the track gate, got "
                   + juce::String (loaded.trackAGate[0].load()));
        check (loaded.uiStateVersion.load() != versionBefore,
               "load: the state version moves so an open editor resyncs");
    }

    //--------------------------------------------------------------------------
    // Track velocity and LPF behave like the track gate
    std::printf ("\nCase 13: track velocity and LPF set every beat on their track\n");
    {
        PolyrhythmProcessor proc;
        setParam (proc, "trackA_velocity", 0.3f);
        setParam (proc, "trackB_lpf",      0.25f);

        bool velSet = true, lpfSet = true;
        for (const auto& v : proc.trackAVelocity) velSet = velSet && std::abs (v.load() - 0.3f)  < 1.0e-4f;
        for (const auto& c : proc.trackBCutoff)   lpfSet = lpfSet && std::abs (c.load() - 0.25f) < 1.0e-4f;

        check (velSet, "velocity: every track A beat velocity follows the parameter");
        check (lpfSet, "lpf: every track B beat cutoff follows the parameter");
        check (std::abs (proc.trackBVelocity[0].load() - 0.8f) < 1.0e-4f,
               "velocity: track B is left alone");
        check (std::abs (proc.trackACutoff[0].load() - 1.0f) < 1.0e-4f,
               "lpf: track A is left alone");
    }

    //--------------------------------------------------------------------------
    std::printf ("\nCase 14: scale lock snaps, steps by degree and is saved\n");
    {
        PolyrhythmProcessor proc;
        proc.scaleRoot.store (5);   // F
        proc.scaleType.store (3);   // Phrygian: F Gb Ab Bb C Db Eb

        check (proc.stepNote (40, 1) == 41, "unlocked: steps are semitones");

        proc.scaleEnabled.store (true);
        check (proc.snapToScale (40) == 39, "snap: E ties between Eb and F and goes down to Eb");
        check (proc.snapToScale (36) == 36, "snap: C is in F Phrygian and stays");
        check (proc.stepNote (36, 1) == 37, "step: C up one degree is Db");
        check (proc.stepNote (37, 1) == 39, "step: Db up one degree is Eb, skipping D");
        check (proc.stepNote (41, -2) == 37, "step: F down two degrees is Db");
        check (proc.stepNote (127, 1) == 127, "step: stays put with no scale note above");

        proc.trackANotes[0].store (40);
        proc.trackBNotes[0].store (38);   // D, not in the scale
        proc.snapAllNotesToScale();
        check (proc.trackANotes[0].load() == 39 && proc.trackBNotes[0].load() == 37,
               "snap all: both tracks move into the scale");

        juce::MemoryBlock saved;
        proc.getStateInformation (saved);
        PolyrhythmProcessor loaded;
        loaded.setStateInformation (saved.getData(), (int) saved.getSize());
        check (loaded.scaleEnabled.load() && loaded.scaleRoot.load() == 5 && loaded.scaleType.load() == 3,
               "load: scale lock, root and type are restored");
    }

    //--------------------------------------------------------------------------
    std::printf ("\nCase 15: randomise changes only the chosen values on the beats in use\n");
    {
        using RT = PolyrhythmProcessor::RandomTarget;
        PolyrhythmProcessor proc;   // track A: 4 beats, track B: 3
        proc.trackAActive[1].store (false);

        proc.randomiseTrack (true, RT::velocity);

        bool velChanged = false, othersKept = true;
        for (size_t i = 0; i < 4; ++i)
        {
            const float v = proc.trackAVelocity[i].load();
            velChanged = velChanged || std::abs (v - 0.8f) > 1.0e-4f;
            othersKept = othersKept && v >= 0.35f && v <= 1.0f
                         && proc.trackANotes[i].load() == 36
                         && std::abs (proc.trackAGate[i].load() - 0.5f) < 1.0e-4f
                         && std::abs (proc.trackACutoff[i].load() - 1.0f) < 1.0e-4f;
        }
        check (velChanged, "velocity: at least one track A velocity changed");
        check (othersKept, "velocity: in range, and notes, gate and LPF untouched");
        check (! proc.trackAActive[1].load() && proc.trackAActive[0].load(),
               "velocity: active steps are kept");
        check (std::abs (proc.trackAVelocity[5].load() - 0.8f) < 1.0e-4f,
               "velocity: beats past the beat count are left alone");
        check (std::abs (proc.trackBVelocity[0].load() - 0.8f) < 1.0e-4f,
               "velocity: track B is left alone");

        proc.scaleRoot.store (5);
        proc.scaleType.store (3);
        proc.scaleEnabled.store (true);
        proc.randomiseTrack (false, RT::all);

        bool inKey = true, inRange = true;
        for (size_t i = 0; i < 3; ++i)
        {
            const int n = proc.trackBNotes[i].load();
            inKey   = inKey && proc.isInScale (n);
            inRange = inRange && n >= 38 - 7 && n <= 38 + 7;
        }
        check (inKey,   "all: every randomised note is in the locked scale");
        check (inRange, "all: notes stay within about an octave of where they were");
        check (proc.getTrackBBeatCount() == 3, "all: the beat count is unchanged");
    }

    //--------------------------------------------------------------------------
    /*  A rhythm switched off starts no notes, and switching it off mid-note still lets the
        held note close, so nothing hangs on the instrument.
    */
    std::printf ("\nCase 16: a rhythm switched off is silent and leaves no hanging notes\n");
    {
        FakePlayHead ph; ph.bpm = bpm;
        std::unique_ptr<PolyrhythmProcessor> proc;
        makeProcessor (proc, ph);

        // Rhythm B's downbeat is held when it is switched off a quarter of a beat in
        auto before = runTransport (*proc, ph, 0.25, blockSize, sampleRate);
        setParam (*proc, "trackB_on", 0.0f);
        auto after  = runTransport (*proc, ph, twoBars - 0.25, blockSize, sampleRate);

        int bOnsAfter = 0, bOns = 0, bOffs = 0;
        for (const auto* cap : { &before, &after })
            for (const auto& e : cap->noteEvents)
                if (e.channel == 2)
                {
                    (e.on ? bOns : bOffs) += 1;
                    if (e.on && cap == &after) ++bOnsAfter;
                }

        check (bOnsAfter == 0,
               "on/off: no rhythm B notes once it is off, got " + juce::String (bOnsAfter));
        check (bOns == 1 && bOffs == 1,
               "on/off: the note held when it went off still closes ("
                   + juce::String (bOns) + " on, " + juce::String (bOffs) + " off)");
        check (after.onsPerChannel[1] + before.onsPerChannel[1] == 8,
               "on/off: rhythm A carries on, got "
                   + juce::String (after.onsPerChannel[1] + before.onsPerChannel[1]));
    }

    //--------------------------------------------------------------------------
    /*  Each rhythm on its own port. Two virtual MIDI destinations stand in for IAC buses,
        so this checks what actually arrives at each port rather than what was queued.
    */
    std::printf ("\nCase 17: each rhythm reaches only its own MIDI port\n");
    {
        struct Collector : juce::MidiInputCallback
        {
            juce::CriticalSection lock;
            std::map<int, int> onsPerChannel;

            void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage& m) override
            {
                if (! m.isNoteOn()) return;
                const juce::ScopedLock sl (lock);
                ++onsPerChannel[m.getChannel()];
            }

            std::map<int, int> snapshot() { const juce::ScopedLock sl (lock); return onsPerChannel; }
        };

        Collector gotA, gotB;
        auto portA = juce::MidiInput::createNewDevice ("PolyTest A", &gotA);
        auto portB = juce::MidiInput::createNewDevice ("PolyTest B", &gotB);

        // CoreMIDI announces a new device on the main run loop, which this test never
        // runs on its own, so spin it briefly until the device shows up
        auto findOutput = [] (const juce::String& name)
        {
            for (int attempt = 0; attempt < 20; ++attempt)
            {
                for (const auto& d : juce::MidiOutput::getAvailableDevices())
                    if (d.name == name)
                        return d.identifier;

               #if JUCE_MAC
                CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.05, false);
               #else
                juce::Thread::sleep (50);
               #endif
            }
            return juce::String();
        };

        const auto idA = findOutput ("PolyTest A");
        const auto idB = findOutput ("PolyTest B");

        if (portA == nullptr || portB == nullptr || idA.isEmpty() || idB.isEmpty())
        {
            std::printf ("  [SKIP] virtual MIDI ports are not available here\n");
        }
        else
        {
            portA->start();
            portB->start();

            {
                FakePlayHead ph; ph.bpm = bpm;
                std::unique_ptr<PolyrhythmProcessor> proc;
                makeProcessor (proc, ph);

                proc->setTrackMidiOutputDevice (true,  idA);
                proc->setTrackMidiOutputDevice (false, idB);

                runTransport (*proc, ph, twoBars, blockSize, sampleRate);
                juce::Thread::sleep (1500);   // let the sender thread deliver everything

                auto a = gotA.snapshot();
                auto b = gotB.snapshot();

                check (a[1] == 8 && a[2] == 0,
                       "ports: rhythm A's port gets only rhythm A (" + juce::String (a[1])
                           + " on ch 1, " + juce::String (a[2]) + " on ch 2)");
                check (b[2] == 6 && b[1] == 0,
                       "ports: rhythm B's port gets only rhythm B (" + juce::String (b[2])
                           + " on ch 2, " + juce::String (b[1]) + " on ch 1)");

                // The ports are saved with the project and reopened on load
                juce::MemoryBlock saved;
                proc->getStateInformation (saved);

                PolyrhythmProcessor loaded;
                loaded.setStateInformation (saved.getData(), (int) saved.getSize());

                check (loaded.getTrackMidiOutputIdentifier (true) == idA
                           && loaded.getTrackMidiOutputIdentifier (false) == idB,
                       "ports: each rhythm's port is restored from the project");
            }

            //------------------------------------------------------------------
            // A rhythm without a port of its own follows the main port
            Collector gotMain;
            auto portMain = juce::MidiInput::createNewDevice ("PolyTest Main", &gotMain);
            const auto idMain = findOutput ("PolyTest Main");

            if (portMain != nullptr && idMain.isNotEmpty())
            {
                portMain->start();
                const auto beforeA = gotA.snapshot();

                FakePlayHead ph; ph.bpm = bpm;
                std::unique_ptr<PolyrhythmProcessor> proc;
                makeProcessor (proc, ph);

                proc->setMidiOutputDevice (idMain);
                proc->setTrackMidiOutputDevice (true, idA);

                runTransport (*proc, ph, twoBars, blockSize, sampleRate);
                juce::Thread::sleep (1500);

                auto main = gotMain.snapshot();
                auto a    = gotA.snapshot();

                check (main[2] == 6 && main[1] == 0,
                       "ports: rhythm B falls back to the main port (" + juce::String (main[2])
                           + " on ch 2, " + juce::String (main[1]) + " on ch 1)");
                check (a[1] - beforeA.at (1) == 8,
                       "ports: rhythm A still uses its own port");
            }
        }
    }

    //--------------------------------------------------------------------------
    std::printf ("\n%s (%d failure%s)\n",
                 failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED",
                 failures, failures == 1 ? "" : "s");

    return failures == 0 ? 0 : 1;
}
