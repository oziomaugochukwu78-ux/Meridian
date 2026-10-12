// "Meridian Test Gain": a minimal VST3 used by the self test to verify plug-in
// hosting end to end. It applies an exact, known gain (default -6 dB), so the
// host's insert path, bypass, state save/restore and undo can be checked with
// numbers rather than ears.

#include <juce_audio_processors/juce_audio_processors.h>

class TestGainProcessor final : public juce::AudioProcessor
{
public:
    TestGainProcessor()
        : AudioProcessor (BusesProperties()
                              .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                              .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
    {
        addParameter (gainDb = new juce::AudioParameterFloat (juce::ParameterID { "gainDb", 1 }, "Gain",
                                                              juce::NormalisableRange<float> (-24.0f, 24.0f), -6.0f));
    }

    bool isBusesLayoutSupported (const BusesLayout& layout) const override
    {
        const auto in = layout.getMainInputChannelSet();
        return in == layout.getMainOutputChannelSet()
               && (in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo());
    }

    void prepareToPlay (double, int) override {}
    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        buffer.applyGain (juce::Decibels::decibelsToGain (gainDb->get()));
    }

    void getStateInformation (juce::MemoryBlock& data) override
    {
        juce::MemoryOutputStream (data, false).writeFloat (gainDb->get());
    }

    void setStateInformation (const void* data, int size) override
    {
        if (size >= (int) sizeof (float))
            *gainDb = juce::MemoryInputStream (data, (size_t) size, false).readFloat();
    }

    const juce::String getName() const override             { return "Meridian Test Gain"; }
    bool acceptsMidi() const override                        { return false; }
    bool producesMidi() const override                       { return false; }
    double getTailLengthSeconds() const override             { return 0.0; }
    int getNumPrograms() override                            { return 1; }
    int getCurrentProgram() override                         { return 0; }
    void setCurrentProgram (int) override                    {}
    const juce::String getProgramName (int) override         { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    bool hasEditor() const override                          { return false; }
    juce::AudioProcessorEditor* createEditor() override      { return nullptr; }

private:
    juce::AudioParameterFloat* gainDb = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TestGainProcessor)
};

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new TestGainProcessor();
}
