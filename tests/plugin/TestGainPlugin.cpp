// "Meridian Test Gain": a minimal VST3 used by the self test to verify plug-in
// hosting end to end. It applies an exact, known gain (default -6 dB) and an
// optional latency: the audio is delayed by N samples and N is reported to the
// host, like a look-ahead limiter or linear-phase EQ. That lets the self test
// check delay compensation with numbers rather than ears.

#include <juce_audio_processors/juce_audio_processors.h>

class TestGainProcessor final : public juce::AudioProcessor,
                                private juce::AudioProcessorParameter::Listener
{
public:
    static constexpr int maxLatency = 8192;

    TestGainProcessor()
        : AudioProcessor (BusesProperties()
                              .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                              .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
    {
        addParameter (gainDb = new juce::AudioParameterFloat (juce::ParameterID { "gainDb", 1 }, "Gain",
                                                              juce::NormalisableRange<float> (-24.0f, 24.0f), -6.0f));
        addParameter (latency = new juce::AudioParameterInt (juce::ParameterID { "latency", 1 }, "Latency (samples)",
                                                             0, maxLatency, 0));
        latency->addListener (this);
        for (auto& line : delayLines)
            line.assign (maxLatency + 1, 0.0f);
    }

    ~TestGainProcessor() override { latency->removeListener (this); }

    bool isBusesLayoutSupported (const BusesLayout& layout) const override
    {
        const auto in = layout.getMainInputChannelSet();
        return in == layout.getMainOutputChannelSet()
               && (in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo());
    }

    void prepareToPlay (double, int) override
    {
        for (auto& line : delayLines)
            std::fill (line.begin(), line.end(), 0.0f);
        writeIndex = 0;
        setLatencySamples (latency->get());
    }

    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        const int delay = juce::jlimit (0, maxLatency, latency->get());
        const int size = maxLatency + 1;
        const int channels = juce::jmin (2, buffer.getNumChannels());

        for (int ch = 0; ch < channels; ++ch)
        {
            auto& line = delayLines[(size_t) ch];
            float* data = buffer.getWritePointer (ch);
            int w = writeIndex;
            for (int i = 0; i < buffer.getNumSamples(); ++i)
            {
                line[(size_t) w] = data[i];
                int r = w - delay;
                if (r < 0)
                    r += size;
                data[i] = line[(size_t) r];
                if (++w == size)
                    w = 0;
            }
        }
        writeIndex = (writeIndex + buffer.getNumSamples()) % size;

        buffer.applyGain (juce::Decibels::decibelsToGain (gainDb->get()));
    }

    void getStateInformation (juce::MemoryBlock& data) override
    {
        juce::MemoryOutputStream stream (data, false);
        stream.writeFloat (gainDb->get());
        stream.writeInt (latency->get());
    }

    void setStateInformation (const void* data, int size) override
    {
        juce::MemoryInputStream stream (data, (size_t) size, false);
        if (size >= (int) sizeof (float))
            *gainDb = stream.readFloat();
        if (size >= (int) (sizeof (float) + sizeof (int)))
            *latency = stream.readInt();
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
    // Report latency changes to the host as soon as the parameter moves.
    void parameterValueChanged (int, float) override { setLatencySamples (latency->get()); }
    void parameterGestureChanged (int, bool) override {}

    juce::AudioParameterFloat* gainDb = nullptr;
    juce::AudioParameterInt* latency = nullptr;
    std::vector<float> delayLines[2];
    int writeIndex = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TestGainProcessor)
};

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new TestGainProcessor();
}
