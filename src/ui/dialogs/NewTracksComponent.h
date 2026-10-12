#pragma once

#include "app/AppController.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace meridian
{

/** Track > New: "Create [1] new [Mono] [Audio Track] named [Audio]". */
class NewTracksComponent final : public juce::Component
{
public:
    static void show (AppController&);

    explicit NewTracksComponent (AppController&);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void create();

    AppController& app;
    juce::Label createLabel, newLabel, namedLabel;
    juce::TextEditor count, baseName;
    juce::ComboBox format, kind;
    juce::TextButton cancelButton { "Cancel" }, createButton { "Create" };
};

} // namespace meridian
