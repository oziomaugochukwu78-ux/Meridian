#include "MeridianLookAndFeel.h"

#include "Theme.h"

namespace meridian
{

MeridianLookAndFeel::MeridianLookAndFeel()
{
    setColourScheme ({ theme::window, theme::panel, theme::panelRaised, theme::textDim, theme::text,
                       theme::panelRaised, theme::textOnLight, theme::selectionEdge, theme::text });

    using namespace juce;
    setColour (ResizableWindow::backgroundColourId, theme::window);
    setColour (DocumentWindow::backgroundColourId, theme::window);

    setColour (TextButton::buttonColourId, theme::panelRaised);
    setColour (TextButton::buttonOnColourId, theme::selectionEdge);
    setColour (TextButton::textColourOffId, theme::text);
    setColour (TextButton::textColourOnId, theme::textOnLight);

    setColour (Label::textColourId, theme::text);
    setColour (Label::backgroundColourId, Colours::transparentBlack);
    setColour (Label::outlineWhenEditingColourId, theme::selectionEdge);

    setColour (ComboBox::backgroundColourId, theme::well);
    setColour (ComboBox::textColourId, theme::text);
    setColour (ComboBox::outlineColourId, theme::divider);
    setColour (ComboBox::arrowColourId, theme::textDim);

    setColour (PopupMenu::backgroundColourId, theme::panel);
    setColour (PopupMenu::textColourId, theme::text);
    setColour (PopupMenu::highlightedBackgroundColourId, theme::selectionEdge);
    setColour (PopupMenu::highlightedTextColourId, theme::textOnLight);
    setColour (PopupMenu::headerTextColourId, theme::textDim);

    setColour (TextEditor::backgroundColourId, theme::well);
    setColour (TextEditor::textColourId, theme::text);
    setColour (TextEditor::outlineColourId, theme::divider);
    setColour (TextEditor::focusedOutlineColourId, theme::selectionEdge);
    setColour (TextEditor::highlightColourId, theme::selection);

    setColour (Slider::backgroundColourId, theme::well);  // the unfilled part of a horizontal slider's track
    setColour (Slider::trackColourId, theme::selectionEdge);
    setColour (Slider::thumbColourId, theme::text);
    setColour (Slider::textBoxTextColourId, theme::text);
    setColour (Slider::textBoxBackgroundColourId, theme::well);
    setColour (Slider::textBoxOutlineColourId, theme::divider);

    setColour (ToggleButton::textColourId, theme::text);
    setColour (ToggleButton::tickColourId, theme::text);
    setColour (ToggleButton::tickDisabledColourId, theme::textDim);

    setColour (ListBox::backgroundColourId, theme::well);
    setColour (ListBox::textColourId, theme::text);
    setColour (ScrollBar::thumbColourId, theme::panelHover);
    setColour (TooltipWindow::backgroundColourId, theme::panelRaised);
    setColour (TooltipWindow::textColourId, theme::text);
    setColour (AlertWindow::backgroundColourId, theme::panel);
    setColour (AlertWindow::textColourId, theme::text);

    setColour (TableHeaderComponent::backgroundColourId, theme::panel);
    setColour (TableHeaderComponent::textColourId, theme::text);
    setColour (TableHeaderComponent::outlineColourId, theme::divider);
}

void MeridianLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour& backgroundColour,
                                                bool isHighlighted, bool isDown)
{
    auto bounds = button.getLocalBounds().toFloat().reduced (0.5f);
    auto fill = backgroundColour;

    if (! button.isEnabled())
        fill = fill.withMultipliedAlpha (0.45f);
    else if (isDown)
        fill = fill.darker (0.25f);
    else if (isHighlighted)
        fill = fill.brighter (0.12f);

    g.setColour (fill);
    g.fillRoundedRectangle (bounds, 2.0f);
    g.setColour (theme::divider);
    g.drawRoundedRectangle (bounds, 2.0f, 1.0f);
}

juce::Font MeridianLookAndFeel::getTextButtonFont (juce::TextButton&, int buttonHeight)
{
    return theme::font (juce::jmin (13.0f, (float) buttonHeight * 0.6f), true);
}

int MeridianLookAndFeel::getSliderThumbRadius (juce::Slider& slider)
{
    return slider.isVertical() ? 14 : LookAndFeel_V4::getSliderThumbRadius (slider);
}

void MeridianLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
                                            float minSliderPos, float maxSliderPos, juce::Slider::SliderStyle style,
                                            juce::Slider& slider)
{
    if (style != juce::Slider::LinearVertical)
    {
        LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos, style, slider);
        return;
    }

    // Recessed fader slot
    const auto centreX = (float) x + (float) width * 0.5f;
    const juce::Rectangle<float> slot (centreX - 3.0f, (float) y, 6.0f, (float) height);
    g.setColour (theme::well);
    g.fillRoundedRectangle (slot, 3.0f);
    g.setColour (theme::divider);
    g.drawRoundedRectangle (slot, 3.0f, 1.0f);

    // Fader cap with a centre groove
    const float capWidth = juce::jmin ((float) width - 4.0f, 30.0f);
    const float capHeight = 26.0f;
    const juce::Rectangle<float> cap (centreX - capWidth * 0.5f, sliderPos - capHeight * 0.5f, capWidth, capHeight);

    juce::ColourGradient body (juce::Colour (0xff5a5a5a), cap.getX(), cap.getY(),
                               juce::Colour (0xff2f2f2f), cap.getX(), cap.getBottom(), false);
    g.setGradientFill (body);
    g.fillRoundedRectangle (cap, 2.5f);
    g.setColour (theme::divider);
    g.drawRoundedRectangle (cap, 2.5f, 1.0f);

    g.setColour (slider.isMouseButtonDown() ? theme::selectionEdge : theme::text);
    g.fillRect (cap.withSizeKeepingCentre (capWidth - 6.0f, 2.0f));
}

void MeridianLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float proportion,
                                            float startAngle, float endAngle, juce::Slider& slider)
{
    const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (3.0f);
    const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto centre = bounds.getCentre();
    const float angle = startAngle + proportion * (endAngle - startAngle);
    const float centreAngle = (startAngle + endAngle) * 0.5f;

    // Value arc, from the centre position (pan-style) to the current angle.
    juce::Path arc;
    arc.addCentredArc (centre.x, centre.y, radius, radius, 0.0f, juce::jmin (centreAngle, angle), juce::jmax (centreAngle, angle), true);
    g.setColour (slider.findColour (juce::Slider::rotarySliderFillColourId, true).isTransparent()
                     ? theme::selectionEdge
                     : slider.findColour (juce::Slider::rotarySliderFillColourId));
    g.strokePath (arc, juce::PathStrokeType (2.5f));

    // Knob body and pointer
    const float bodyRadius = radius - 4.0f;
    g.setColour (theme::panelRaised);
    g.fillEllipse (centre.x - bodyRadius, centre.y - bodyRadius, bodyRadius * 2.0f, bodyRadius * 2.0f);
    g.setColour (theme::divider);
    g.drawEllipse (centre.x - bodyRadius, centre.y - bodyRadius, bodyRadius * 2.0f, bodyRadius * 2.0f, 1.0f);

    juce::Path pointer;
    pointer.addRectangle (-1.0f, -bodyRadius + 2.0f, 2.0f, bodyRadius * 0.55f);
    g.setColour (theme::text);
    g.fillPath (pointer, juce::AffineTransform::rotation (angle).translated (centre));
}

void MeridianLookAndFeel::drawComboBox (juce::Graphics& g, int width, int height, bool, int, int, int, int, juce::ComboBox& box)
{
    const auto bounds = juce::Rectangle<int> (0, 0, width, height).toFloat().reduced (0.5f);
    g.setColour (box.findColour (juce::ComboBox::backgroundColourId));
    g.fillRoundedRectangle (bounds, 2.0f);
    g.setColour (box.findColour (juce::ComboBox::outlineColourId));
    g.drawRoundedRectangle (bounds, 2.0f, 1.0f);

    juce::Path arrow;
    const float ax = (float) width - 10.0f;
    const float ay = (float) height * 0.5f;
    arrow.addTriangle (ax - 3.5f, ay - 2.0f, ax + 3.5f, ay - 2.0f, ax, ay + 2.5f);
    g.setColour (box.findColour (juce::ComboBox::arrowColourId).withMultipliedAlpha (box.isEnabled() ? 1.0f : 0.4f));
    g.fillPath (arrow);
}

juce::Font MeridianLookAndFeel::getComboBoxFont (juce::ComboBox&) { return theme::font (12.0f); }
juce::Font MeridianLookAndFeel::getPopupMenuFont()                { return theme::font (13.0f); }
juce::Font MeridianLookAndFeel::getLabelFont (juce::Label& label) { return label.getFont(); }

} // namespace meridian
