// "Meridian Test Synth": a minimal VST3 instrument used by the self test to
// verify instrument hosting end to end. Each note is a pure sine at its exact
// equal-tempered frequency (A4 = 440 Hz), amplitude velocity / 2, starting at
// zero phase on its MIDI event's sample and stopping dead at the note-off: no
// envelope, so the self test can see exactly where every note starts and ends.
// The sustain pedal (CC 64) holds released notes until it lifts, stopping dead there.

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <cmath>

class TestSynthProcessor final : public juce::AudioProcessor
{
public:
    TestSynthProcessor()
        : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true))
    {
    }

    bool isBusesLayoutSupported (const BusesLayout& layout) const override
    {
        const auto out = layout.getMainOutputChannelSet();
        return layout.getMainInputChannelSet().isDisabled()
               && (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo());
    }

    void prepareToPlay (double rate, int) override
    {
        sampleRate = rate;
        pedal = false;
        for (auto& v : voices)
            v = {};
    }

    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        buffer.clear();
        int done = 0;
        for (const auto metadata : midi)
        {
            render (buffer, done, metadata.samplePosition);
            done = metadata.samplePosition;
            handle (metadata.getMessage());
        }
        render (buffer, done, buffer.getNumSamples());
    }

    const juce::String getName() const override             { return "Meridian Test Synth"; }
    bool acceptsMidi() const override                        { return true; }
    bool producesMidi() const override                       { return false; }
    double getTailLengthSeconds() const override             { return 0.0; }
    int getNumPrograms() override                            { return 1; }
    int getCurrentProgram() override                         { return 0; }
    void setCurrentProgram (int) override                    {}
    const juce::String getProgramName (int) override         { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    bool hasEditor() const override                          { return false; }
    juce::AudioProcessorEditor* createEditor() override      { return nullptr; }
    void getStateInformation (juce::MemoryBlock&) override   {}
    void setStateInformation (const void*, int) override     {}

private:
    struct Voice
    {
        int note = -1;
        bool sustained = false; // released, held by the pedal
        double phase = 0.0, increment = 0.0, amplitude = 0.0;
    };

    void handle (const juce::MidiMessage& m)
    {
        if (m.isNoteOn())
        {
            for (auto& v : voices)
                if (v.note < 0)
                {
                    v.note = m.getNoteNumber();
                    v.sustained = false;
                    v.phase = 0.0;
                    v.increment = 2.0 * juce::MathConstants<double>::pi * 440.0 * std::pow (2.0, (v.note - 69) / 12.0) / sampleRate;
                    v.amplitude = 0.5 * m.getFloatVelocity();
                    break;
                }
        }
        else if (m.isNoteOff())
        {
            for (auto& v : voices)
                if (v.note == m.getNoteNumber())
                {
                    if (pedal)
                        v.sustained = true;
                    else
                        v.note = -1;
                }
        }
        else if (m.isSustainPedalOn())
        {
            pedal = true;
        }
        else if (m.isSustainPedalOff())
        {
            pedal = false;
            for (auto& v : voices)
                if (v.sustained)
                    v = {};
        }
        else if (m.isAllNotesOff() || m.isAllSoundOff())
        {
            pedal = false;
            for (auto& v : voices)
                v = {};
        }
    }

    void render (juce::AudioBuffer<float>& buffer, int from, int to)
    {
        for (auto& v : voices)
        {
            if (v.note < 0)
                continue;
            for (int i = from; i < to; ++i)
            {
                const auto x = (float) (v.amplitude * std::sin (v.phase));
                for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                    buffer.addSample (ch, i, x);
                v.phase += v.increment;
            }
        }
    }

    double sampleRate = 48000.0;
    bool pedal = false;
    std::array<Voice, 8> voices {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TestSynthProcessor)
};

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new TestSynthProcessor();
}
