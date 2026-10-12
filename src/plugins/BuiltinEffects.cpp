#include "BuiltinEffects.h"

#include <dsp/FdnReverb.h>
#include <dsp/ParametricEq.h>

namespace meridian
{
namespace
{
constexpr const char* eqId = "meridian:eq";
constexpr const char* reverbId = "meridian:reverb";

juce::NormalisableRange<float> frequencyRange (float lo, float hi, float centre)
{
    juce::NormalisableRange<float> r (lo, hi);
    r.setSkewForCentre (centre); // logarithmic-feeling frequency knobs
    return r;
}

//==============================================================================
/** A parameter of a built-in effect: a real-world range (Hz, dB, seconds...)
    mapped to the host's normalised 0..1, readable from the audio thread.
    (Plug-in instances must expose HostedParameters, so the stock JUCE
    parameter classes cannot be used here.) */
class EffectParameter final : public juce::AudioPluginInstance::HostedParameter
{
public:
    EffectParameter (juce::String parameterId, juce::String displayName, juce::NormalisableRange<float> valueRange,
                     float defaultRealValue, juce::String unitLabel, bool toggle = false)
        : range (valueRange), id (std::move (parameterId)), name (std::move (displayName)),
          unit (std::move (unitLabel)), isToggle (toggle), defaultValue (range.convertTo0to1 (defaultRealValue))
    {
        normalised.store (defaultValue);
    }

    float get() const noexcept { return range.convertFrom0to1 (normalised.load (std::memory_order_relaxed)); }
    bool isOn() const noexcept { return normalised.load (std::memory_order_relaxed) >= 0.5f; }

    float getValue() const override { return normalised.load(); }
    void setValue (float v) override { normalised.store (juce::jlimit (0.0f, 1.0f, v)); }
    float getDefaultValue() const override { return defaultValue; }
    juce::String getName (int maximumLength) const override { return name.substring (0, maximumLength); }
    juce::String getLabel() const override { return unit; }
    juce::String getParameterID() const override { return id; }
    bool isBoolean() const override { return isToggle; }
    int getNumSteps() const override { return isToggle ? 2 : juce::AudioProcessor::getDefaultNumParameterSteps(); }

    juce::String getText (float v, int) const override
    {
        if (isToggle)
            return v >= 0.5f ? "On" : "Off";
        const float real = range.convertFrom0to1 (juce::jlimit (0.0f, 1.0f, v));
        return juce::String (real, std::abs (real) < 10.0f ? 2 : std::abs (real) < 100.0f ? 1 : 0) + (unit.isEmpty() ? "" : " " + unit);
    }

    float getValueForText (const juce::String& text) const override
    {
        if (isToggle)
            return text.trim().equalsIgnoreCase ("on") || text.getFloatValue() >= 0.5f ? 1.0f : 0.0f;
        return range.convertTo0to1 (juce::jlimit (range.start, range.end, text.getFloatValue()));
    }

    const juce::NormalisableRange<float> range;

private:
    juce::String id, name, unit;
    bool isToggle = false;
    float defaultValue = 0.0f;
    std::atomic<float> normalised { 0.0f };
};

//==============================================================================
/** Shared plumbing: mono or stereo (same in and out), parameter state as XML,
    the generic editor, one program. */
class BuiltinEffect : public juce::AudioPluginInstance
{
public:
    BuiltinEffect (juce::String effectName, juce::String effectCategory, juce::String effectId)
        : juce::AudioPluginInstance (BusesProperties()
                                         .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                         .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
          name (std::move (effectName)), category (std::move (effectCategory)), identifier (std::move (effectId))
    {
    }

    const juce::String getName() const override { return name; }

    void fillInPluginDescription (juce::PluginDescription& d) const override
    {
        d.name = name;
        d.descriptiveName = name;
        d.pluginFormatName = BuiltinEffectFormat::formatName;
        d.category = category;
        d.manufacturerName = "Meridian";
        d.version = "1.0";
        d.fileOrIdentifier = identifier;
        d.uniqueId = (int) (identifier.hashCode() & 0x7fffffff);
        d.deprecatedUid = d.uniqueId;
        d.isInstrument = false;
        d.numInputChannels = 2;
        d.numOutputChannels = 2;
    }

    bool isBusesLayoutSupported (const BusesLayout& layout) const override
    {
        const auto in = layout.getMainInputChannelSet(), out = layout.getMainOutputChannelSet();
        return in == out && (in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo());
    }

    void releaseResources() override {}
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override { return new juce::GenericAudioProcessorEditor (*this); }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return "Default"; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& data) override
    {
        // Real-world values by parameter id: readable, and stable if ranges change later.
        juce::XmlElement xml ("MERIDIAN_EFFECT");
        xml.setAttribute ("id", identifier);
        for (auto* p : getParameters())
            if (auto* e = dynamic_cast<EffectParameter*> (p))
                xml.setAttribute (e->getParameterID(), (double) e->get());
        copyXmlToBinary (xml, data);
    }

    void setStateInformation (const void* data, int size) override
    {
        if (auto xml = getXmlFromBinary (data, size))
            for (auto* p : getParameters())
                if (auto* e = dynamic_cast<EffectParameter*> (p))
                    if (xml->hasAttribute (e->getParameterID()))
                    {
                        const auto real = juce::jlimit (e->range.start, e->range.end, (float) xml->getDoubleAttribute (e->getParameterID()));
                        e->setValueNotifyingHost (e->range.convertTo0to1 (real));
                    }
    }

protected:
    EffectParameter* add (juce::String id, juce::String displayName, juce::NormalisableRange<float> range,
                          float defaultValue, juce::String unit, bool toggle = false)
    {
        auto parameter = std::make_unique<EffectParameter> (std::move (id), std::move (displayName), range, defaultValue,
                                                            std::move (unit), toggle);
        auto* raw = parameter.get();
        addHostedParameter (std::move (parameter));
        return raw;
    }

    static float value (const EffectParameter* p) { return p->get(); }

private:
    juce::String name, category, identifier;
};

//==============================================================================
class MeridianEq final : public BuiltinEffect
{
public:
    MeridianEq() : BuiltinEffect ("Meridian EQ", "EQ", eqId)
    {
        highPassOn = add ("hpOn", "High-Pass On", { 0.0f, 1.0f, 1.0f }, 0.0f, {}, true);
        highPassHz = add ("hpFreq", "High-Pass Freq", frequencyRange (20.0f, 2000.0f, 150.0f), 80.0f, "Hz");
        lowShelfHz = add ("lsFreq", "Low Shelf Freq", frequencyRange (20.0f, 1000.0f, 150.0f), 100.0f, "Hz");
        lowShelfDb = add ("lsGain", "Low Shelf Gain", { -24.0f, 24.0f }, 0.0f, "dB");
        const float defaults[3] = { 250.0f, 1000.0f, 4000.0f };
        for (int i = 0; i < 3; ++i)
        {
            const auto n = juce::String (i + 1);
            peakHz[i] = add ("p" + n + "Freq", "Band " + n + " Freq", frequencyRange (20.0f, 20000.0f, 1000.0f), defaults[i], "Hz");
            peakDb[i] = add ("p" + n + "Gain", "Band " + n + " Gain", { -24.0f, 24.0f }, 0.0f, "dB");
            peakQ[i] = add ("p" + n + "Q", "Band " + n + " Q", frequencyRange (0.1f, 18.0f, 1.0f), 1.0f, {});
        }
        highShelfHz = add ("hsFreq", "High Shelf Freq", frequencyRange (1000.0f, 20000.0f, 6000.0f), 8000.0f, "Hz");
        highShelfDb = add ("hsGain", "High Shelf Gain", { -24.0f, 24.0f }, 0.0f, "dB");
        lowPassOn = add ("lpOn", "Low-Pass On", { 0.0f, 1.0f, 1.0f }, 0.0f, {}, true);
        lowPassHz = add ("lpFreq", "Low-Pass Freq", frequencyRange (200.0f, 20000.0f, 6000.0f), 18000.0f, "Hz");
        outputDb = add ("output", "Output", { -24.0f, 24.0f }, 0.0f, "dB");
    }

    void prepareToPlay (double sampleRate, int) override
    {
        eq.prepare (sampleRate);
        applied = {};
        applyParameters (true);
    }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        juce::ScopedNoDenormals noDenormals;
        applyParameters (false);
        eq.process (buffer.getArrayOfWritePointers(), buffer.getNumChannels(), buffer.getNumSamples());
    }

    double getTailLengthSeconds() const override { return 0.0; }

private:
    void applyParameters (bool force)
    {
        dsp::ParametricEq::Params p;
        p.highPassOn = highPassOn->isOn();
        p.highPassHz = value (highPassHz);
        p.lowShelf = { value (lowShelfHz), value (lowShelfDb), 1.0 };
        for (size_t i = 0; i < 3; ++i)
            p.peaks[i] = { value (peakHz[i]), value (peakDb[i]), value (peakQ[i]) };
        p.highShelf = { value (highShelfHz), value (highShelfDb), 1.0 };
        p.lowPassOn = lowPassOn->isOn();
        p.lowPassHz = value (lowPassHz);
        p.outputDb = value (outputDb);

        // Recompute coefficients only when a setting actually changed.
        if (force || ! same (p, applied))
        {
            eq.setParams (p);
            applied = p;
        }
    }

    static bool same (const dsp::ParametricEq::Band& a, const dsp::ParametricEq::Band& b)
    {
        return juce::exactlyEqual (a.frequency, b.frequency) && juce::exactlyEqual (a.gainDb, b.gainDb) && juce::exactlyEqual (a.q, b.q);
    }

    static bool same (const dsp::ParametricEq::Params& a, const dsp::ParametricEq::Params& b)
    {
        return a.highPassOn == b.highPassOn && juce::exactlyEqual (a.highPassHz, b.highPassHz) && same (a.lowShelf, b.lowShelf)
            && same (a.peaks[0], b.peaks[0]) && same (a.peaks[1], b.peaks[1]) && same (a.peaks[2], b.peaks[2])
            && same (a.highShelf, b.highShelf) && a.lowPassOn == b.lowPassOn && juce::exactlyEqual (a.lowPassHz, b.lowPassHz)
            && juce::exactlyEqual (a.outputDb, b.outputDb);
    }

    dsp::ParametricEq eq;
    dsp::ParametricEq::Params applied;
    EffectParameter* highPassOn = nullptr;
    EffectParameter* lowPassOn = nullptr;
    EffectParameter* highPassHz = nullptr;
    EffectParameter* lowShelfHz = nullptr;
    EffectParameter* lowShelfDb = nullptr;
    EffectParameter* peakHz[3] {};
    EffectParameter* peakDb[3] {};
    EffectParameter* peakQ[3] {};
    EffectParameter* highShelfHz = nullptr;
    EffectParameter* highShelfDb = nullptr;
    EffectParameter* lowPassHz = nullptr;
    EffectParameter* outputDb = nullptr;
};

//==============================================================================
class MeridianReverb final : public BuiltinEffect
{
public:
    MeridianReverb() : BuiltinEffect ("Meridian Reverb", "Reverb", reverbId)
    {
        size = add ("size", "Size", { 0.3f, 2.0f }, 1.0f, {});
        decay = add ("decay", "Decay", frequencyRange (0.2f, 20.0f, 2.0f), 2.0f, "s");
        damping = add ("damping", "Damping", { 0.0f, 1.0f }, 0.3f, {});
        predelay = add ("predelay", "Pre-delay", { 0.0f, 250.0f }, 10.0f, "ms");
        mix = add ("mix", "Mix", { 0.0f, 1.0f }, 0.25f, {});
        width = add ("width", "Width", { 0.0f, 1.0f }, 1.0f, {});
    }

    void prepareToPlay (double sampleRate, int) override
    {
        reverb.prepare (sampleRate); // allocates: prepareToPlay runs off the audio thread
        applied = {};
        applyParameters (true);
    }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        juce::ScopedNoDenormals noDenormals;
        applyParameters (false);
        reverb.process (buffer.getWritePointer (0), buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : nullptr,
                        buffer.getNumSamples());
    }

    double getTailLengthSeconds() const override { return (double) decay->get() + (double) predelay->get() / 1000.0; }

private:
    void applyParameters (bool force)
    {
        dsp::FdnReverb::Params p;
        p.size = value (size);
        p.decay = value (decay);
        p.damping = value (damping);
        p.predelayMs = value (predelay);
        p.mix = value (mix);
        p.width = value (width);
        const bool changed = ! juce::exactlyEqual (p.size, applied.size) || ! juce::exactlyEqual (p.decay, applied.decay)
                          || ! juce::exactlyEqual (p.damping, applied.damping) || ! juce::exactlyEqual (p.predelayMs, applied.predelayMs)
                          || ! juce::exactlyEqual (p.mix, applied.mix) || ! juce::exactlyEqual (p.width, applied.width);
        if (force || changed)
        {
            reverb.setParams (p);
            applied = p;
        }
    }

    dsp::FdnReverb reverb;
    dsp::FdnReverb::Params applied;
    EffectParameter* size = nullptr;
    EffectParameter* decay = nullptr;
    EffectParameter* damping = nullptr;
    EffectParameter* predelay = nullptr;
    EffectParameter* mix = nullptr;
    EffectParameter* width = nullptr;
};

std::unique_ptr<juce::AudioPluginInstance> createEffect (const juce::String& identifier)
{
    if (identifier == eqId)     return std::make_unique<MeridianEq>();
    if (identifier == reverbId) return std::make_unique<MeridianReverb>();
    return nullptr;
}
} // namespace

//==============================================================================
juce::Array<juce::PluginDescription> BuiltinEffectFormat::getDescriptions()
{
    juce::Array<juce::PluginDescription> result;
    for (const auto* id : { eqId, reverbId })
    {
        juce::PluginDescription d;
        createEffect (id)->fillInPluginDescription (d);
        result.add (d);
    }
    return result;
}

void BuiltinEffectFormat::findAllTypesForFile (juce::OwnedArray<juce::PluginDescription>& results, const juce::String& identifier)
{
    if (auto effect = createEffect (identifier))
    {
        auto* d = results.add (new juce::PluginDescription());
        effect->fillInPluginDescription (*d);
    }
}

juce::String BuiltinEffectFormat::getNameOfPluginFromIdentifier (const juce::String& identifier)
{
    if (auto effect = createEffect (identifier))
        return effect->getName();
    return identifier;
}

juce::StringArray BuiltinEffectFormat::searchPathsForPlugins (const juce::FileSearchPath&, bool, bool)
{
    return { eqId, reverbId };
}

void BuiltinEffectFormat::createPluginInstance (const juce::PluginDescription& description, double sampleRate,
                                                int blockSize, PluginCreationCallback callback)
{
    auto effect = createEffect (description.fileOrIdentifier);
    if (effect == nullptr)
    {
        callback (nullptr, "Unknown built-in effect \"" + description.fileOrIdentifier + "\".");
        return;
    }
    effect->setRateAndBufferSizeDetails (sampleRate, blockSize);
    callback (std::move (effect), {});
}

} // namespace meridian
