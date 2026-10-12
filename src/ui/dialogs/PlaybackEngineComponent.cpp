#include "PlaybackEngineComponent.h"

#include "DialogUtils.h"

namespace meridian
{
namespace
{
constexpr const char* diskCacheKey = "diskCacheFrames";
const int cacheSizes[] = { 32768, 131072, 524288 }; // Normal, Large, Extra Large (frames per stream half)
} // namespace

void PlaybackEngineComponent::show (AppController& app)
{
    dialogs::launch (new PlaybackEngineComponent (app), "Playback Engine");
}

PlaybackEngineComponent::PlaybackEngineComponent (AppController& a) : app (a)
{
    dialogs::styleFieldLabel (engineLabel, "Playback Engine:");
    dialogs::styleFieldLabel (inputLabel, "Input Device:");
    dialogs::styleFieldLabel (bufferLabel, "H/W Buffer Size:");
    dialogs::styleFieldLabel (rateLabel, "Sample Rate:");
    dialogs::styleSectionLabel (hostHeader, "HOST ENGINE");
    dialogs::styleFieldLabel (delayLabel, "Delay Compensation Engine:");
    dialogs::styleSectionLabel (diskHeader, "DISK PLAYBACK");
    dialogs::styleFieldLabel (cacheLabel, "Cache Size:");

    engine.onChange = [this] { applyEngine(); };
    input.onChange = [this] { applyInput(); };
    bufferSize.onChange = [this] { applyBufferSize(); };
    sampleRate.onChange = [this] { applySampleRate(); };

    ignoreErrors.setToggleState (true, juce::dontSendNotification);
    ignoreErrors.setEnabled (false);
    ignoreErrors.setTooltip ("Always on: a disk or CPU overload is reported but never stops the transport.");
    minimizeLatency.setEnabled (false);
    minimizeLatency.setTooltip ("Planned.");
    dynamicPlugins.setEnabled (false);
    dynamicPlugins.setTooltip ("Planned: skip processing for plug-ins on silent tracks.");

    for (const auto* item : { "None", "Short", "Long", "Maximum" })
        delayCompensation.addItem (item, delayCompensation.getNumItems() + 1);
    delayCompensation.setSelectedId (1, juce::dontSendNotification);
    delayCompensation.setEnabled (false);
    delayCompensation.setTooltip ("Plug-in delay compensation is planned. Currently no compensation is applied.");

    cacheSize.addItem ("Normal (0.7 s per stream)", 1);
    cacheSize.addItem ("Large (2.7 s per stream)", 2);
    cacheSize.addItem ("Extra Large (11 s per stream)", 3);
    const int currentCache = app.getBridge().getDiskCacheFrames();
    cacheSize.setSelectedId (currentCache >= cacheSizes[2] ? 3 : currentCache >= cacheSizes[1] ? 2 : 1, juce::dontSendNotification);
    cacheSize.setTooltip ("Read-ahead per audio clip. Larger survives slower drives, at the cost of memory.");
    cacheSize.onChange = [this] { applyCacheSize(); };

    controlPanelButton.onClick = [this]
    {
        if (auto* device = app.getDeviceManager().getCurrentAudioDevice(); device != nullptr && device->hasControlPanel())
            if (device->showControlPanel())
                app.getDeviceManager().restartLastAudioDevice();
    };
    okButton.onClick = [this] { dialogs::close (*this); };

    info.setFont (theme::monoFont (11.0f));
    info.setColour (juce::Label::textColourId, theme::textDim);
    info.setJustificationType (juce::Justification::topLeft);
    error.setFont (theme::font (12.0f));
    error.setColour (juce::Label::textColourId, theme::record);

    for (juce::Component* c : std::initializer_list<juce::Component*> {
             &engineLabel, &engine, &inputLabel, &input, &bufferLabel, &bufferSize, &rateLabel, &sampleRate, &hostHeader,
             &ignoreErrors, &minimizeLatency, &dynamicPlugins, &delayLabel, &delayCompensation, &diskHeader, &cacheLabel,
             &cacheSize, &info, &error, &controlPanelButton, &okButton })
        addAndMakeVisible (c);

    populateEngines();
    app.getDeviceManager().addChangeListener (this);
    setSize (580, 540);
    refresh();
}

PlaybackEngineComponent::~PlaybackEngineComponent()
{
    app.getDeviceManager().removeChangeListener (this);
}

void PlaybackEngineComponent::populateEngines()
{
    choices.clear();
    engine.clear (juce::dontSendNotification);

    for (auto* type : app.getDeviceManager().getAvailableDeviceTypes())
    {
        type->scanForDevices();
        for (const auto& name : type->getDeviceNames (false))
        {
            choices.push_back ({ type->getTypeName(), name });
            engine.addItem (type->getTypeName() + ": " + name, (int) choices.size());
        }
    }

    if (choices.empty())
        engine.addItem ("No audio devices found", 1000);
}

void PlaybackEngineComponent::refresh()
{
    auto& dm = app.getDeviceManager();
    const auto setup = dm.getAudioDeviceSetup();
    auto* device = dm.getCurrentAudioDevice();
    auto* type = dm.getCurrentDeviceTypeObject();

    for (size_t i = 0; i < choices.size(); ++i)
        if (choices[i].typeName == dm.getCurrentAudioDeviceType() && choices[i].deviceName == setup.outputDeviceName)
            engine.setSelectedId ((int) i + 1, juce::dontSendNotification);

    // Separate input device (e.g. Windows Audio); ASIO devices are full duplex.
    const bool separateInputs = type != nullptr && type->hasSeparateInputsAndOutputs();
    input.clear (juce::dontSendNotification);
    inputLabel.setVisible (separateInputs);
    input.setVisible (separateInputs);
    if (separateInputs)
    {
        input.addItem ("None", 1);
        const auto names = type->getDeviceNames (true);
        for (int i = 0; i < names.size(); ++i)
            input.addItem (names[i], i + 2);
        const int index = names.indexOf (setup.inputDeviceName);
        input.setSelectedId (index >= 0 ? index + 2 : 1, juce::dontSendNotification);
    }

    bufferSize.clear (juce::dontSendNotification);
    sampleRate.clear (juce::dontSendNotification);
    controlPanelButton.setEnabled (device != nullptr && device->hasControlPanel());

    if (device == nullptr)
    {
        info.setText ("No audio device is open.", juce::dontSendNotification);
        return;
    }

    for (const int size : device->getAvailableBufferSizes())
        bufferSize.addItem (juce::String (size) + " Samples", size);
    bufferSize.setSelectedId (device->getCurrentBufferSizeSamples(), juce::dontSendNotification);

    for (const double rate : device->getAvailableSampleRates())
        sampleRate.addItem (juce::String (rate / 1000.0, 1) + " kHz", juce::roundToInt (rate));
    sampleRate.setSelectedId (juce::roundToInt (device->getCurrentSampleRate()), juce::dontSendNotification);

    const double sr = device->getCurrentSampleRate();
    const auto ms = [sr] (int samples) { return juce::String (1000.0 * samples / juce::jmax (1.0, sr), 1) + " ms"; };
    const int inLatency = device->getInputLatencyInSamples();
    const int outLatency = device->getOutputLatencyInSamples();
    const int consoleLatency = app.getEngine().getMasterBus().getLatencySamples();

    juce::String text;
    text << "Device:          " << device->getName() << " (" << device->getTypeName() << ")\n"
         << "Active I/O:      " << device->getActiveInputChannels().countNumberOfSetBits() << " in / "
         << device->getActiveOutputChannels().countNumberOfSetBits() << " out\n"
         << "Input latency:   " << inLatency << " samples (" << ms (inLatency) << ")\n"
         << "Output latency:  " << outLatency << " samples (" << ms (outLatency) << ")\n"
         << "Master console:  +" << consoleLatency << " samples (" << ms (consoleLatency) << ")\n"
         << "CPU:             " << juce::roundToInt (app.getDeviceManager().getCpuUsage() * 100.0) << " %";
    info.setText (text, juce::dontSendNotification);

    if (app.getSession() != nullptr && std::abs (sr - app.getSession()->getSampleRate()) > 0.5)
        reportError ("The session is " + juce::String (app.getSession()->getSampleRate() / 1000.0, 1)
                     + " kHz but the device runs at " + juce::String (sr / 1000.0, 1) + " kHz.");

    resized(); // the Input Device row comes and goes with the driver type
}

void PlaybackEngineComponent::reportError (const juce::String& message)
{
    error.setText (message, juce::dontSendNotification);
}

void PlaybackEngineComponent::applyEngine()
{
    const int index = engine.getSelectedId() - 1;
    if (! juce::isPositiveAndBelow (index, (int) choices.size()))
        return;

    auto& dm = app.getDeviceManager();
    const auto& choice = choices[(size_t) index];

    error.setText ({}, juce::dontSendNotification);
    if (dm.getCurrentAudioDeviceType() != choice.typeName)
        dm.setCurrentAudioDeviceType (choice.typeName, true);

    auto setup = dm.getAudioDeviceSetup();
    setup.outputDeviceName = choice.deviceName;
    if (auto* type = dm.getCurrentDeviceTypeObject(); type != nullptr && ! type->hasSeparateInputsAndOutputs())
        setup.inputDeviceName = choice.deviceName;
    setup.useDefaultInputChannels = true;
    setup.useDefaultOutputChannels = true;
    if (auto* session = app.getSession())
        setup.sampleRate = session->getSampleRate();

    if (const auto result = dm.setAudioDeviceSetup (setup, true); result.isNotEmpty())
        reportError (result);
    refresh();
}

void PlaybackEngineComponent::applyInput()
{
    auto& dm = app.getDeviceManager();
    auto setup = dm.getAudioDeviceSetup();
    setup.inputDeviceName = input.getSelectedId() <= 1 ? juce::String() : input.getText();
    setup.useDefaultInputChannels = true;
    if (const auto result = dm.setAudioDeviceSetup (setup, true); result.isNotEmpty())
        reportError (result);
    refresh();
}

void PlaybackEngineComponent::applyBufferSize()
{
    auto& dm = app.getDeviceManager();
    auto setup = dm.getAudioDeviceSetup();
    setup.bufferSize = bufferSize.getSelectedId();
    if (const auto result = dm.setAudioDeviceSetup (setup, true); result.isNotEmpty())
        reportError (result);
    refresh();
}

void PlaybackEngineComponent::applySampleRate()
{
    auto& dm = app.getDeviceManager();
    auto setup = dm.getAudioDeviceSetup();
    setup.sampleRate = (double) sampleRate.getSelectedId();
    error.setText ({}, juce::dontSendNotification);
    if (const auto result = dm.setAudioDeviceSetup (setup, true); result.isNotEmpty())
        reportError (result);
    refresh();
}

void PlaybackEngineComponent::applyCacheSize()
{
    const int frames = cacheSizes[juce::jlimit (0, 2, cacheSize.getSelectedId() - 1)];
    app.getBridge().setDiskCacheFrames (frames);
    app.getSettings().setValue (diskCacheKey, frames);
}

void PlaybackEngineComponent::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);
}

void PlaybackEngineComponent::resized()
{
    auto area = getLocalBounds().reduced (18);
    constexpr int labelWidth = 190;

    auto row = [&] (juce::Label& label, juce::Component& control, int controlWidth)
    {
        auto r = area.removeFromTop (26);
        label.setBounds (r.removeFromLeft (labelWidth));
        r.removeFromLeft (8);
        control.setBounds (r.removeFromLeft (controlWidth).reduced (0, 1));
        area.removeFromTop (6);
    };

    row (engineLabel, engine, 320);
    if (input.isVisible())
        row (inputLabel, input, 320);
    row (bufferLabel, bufferSize, 160);
    row (rateLabel, sampleRate, 160);

    area.removeFromTop (6);
    hostHeader.setBounds (area.removeFromTop (18));
    for (auto* t : { &ignoreErrors, &minimizeLatency, &dynamicPlugins })
        t->setBounds (area.removeFromTop (24).withTrimmedLeft (labelWidth + 8));
    area.removeFromTop (4);
    row (delayLabel, delayCompensation, 160);

    diskHeader.setBounds (area.removeFromTop (18));
    row (cacheLabel, cacheSize, 240);

    auto buttons = area.removeFromBottom (28);
    okButton.setBounds (buttons.removeFromRight (90));
    controlPanelButton.setBounds (buttons.removeFromLeft (140));
    area.removeFromBottom (6);
    error.setBounds (area.removeFromBottom (20));
    info.setBounds (area);
}

} // namespace meridian
