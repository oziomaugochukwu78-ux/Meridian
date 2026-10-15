#include "EditToolbar.h"

#include "ui/Theme.h"

namespace meridian
{
namespace
{
struct ModeInfo { const char* name; EditMode mode; bool available; const char* tooltip; };
struct ToolInfo { const char* name; EditTool tool; bool available; const char* tooltip; };

const ModeInfo modes[] = {
    { "SHUFFLE", EditMode::shuffle, true,  "Shuffle mode: clips stay butted together. Deleting closes the gap, trimming "
                                           "moves later clips, and a moved clip drops in at the nearest clip boundary (F1)" },
    { "SLIP",    EditMode::slip,    true,  "Slip mode: clips move freely (F2)" },
    { "SPOT",    EditMode::spot,    true,  "Spot mode: click a clip to type its exact start time (F3)" },
    { "GRID",    EditMode::grid,    true,  "Grid mode: edits snap to the beat grid (F4)" },
};

const ToolInfo tools[] = {
    { "ZOOM",  EditTool::zoomer,   false, "Zoomer (planned). Use Ctrl+wheel or +/- to zoom" },
    { "TRIM",  EditTool::trim,     true,  "Trim tool: drag clip edges. Alt+drag the right edge to time-stretch (F6)" },
    { "SEL",   EditTool::selector, true,  "Selector: drag to select a time range, click to place the cursor (F7)" },
    { "GRAB",  EditTool::grabber,  true,  "Grabber: move clips, drag onto another track to move between tracks (F8)" },
    { "SMART", EditTool::smart,    true,  "Smart Tool: selector in the upper half of a clip, grabber in the lower half, "
                                          "trim at the edges, fades at the top corners (F6+F7)" },
    { "PENCIL", EditTool::pencil,  true,  "Pencil: draw volume automation. Drag to draw, drag a point to move it, "
                                          "Alt-click a point to delete it (F10)" },
};
} // namespace

EditToolbar::EditToolbar (EditState& s) : state (s)
{
    for (const auto& m : modes)
    {
        auto* b = modeButtons.add (new juce::TextButton (m.name));
        b->setTooltip (m.tooltip);
        b->setEnabled (m.available);
        b->setColour (juce::TextButton::buttonOnColourId, theme::text);
        b->onClick = [this, mode = m.mode] { state.setMode (mode); };
        addAndMakeVisible (b);
    }

    for (const auto& t : tools)
    {
        auto* b = toolButtons.add (new juce::TextButton (t.name));
        b->setTooltip (t.tooltip);
        b->setEnabled (t.available);
        b->setColour (juce::TextButton::buttonOnColourId, theme::selectionEdge);
        b->onClick = [this, tool = t.tool] { state.setTool (tool); };
        addAndMakeVisible (b);
    }

    zoomOut.setTooltip ("Zoom out (Ctrl+[)");
    zoomIn.setTooltip ("Zoom in (Ctrl+])");
    zoomOut.onClick = [this] { state.zoom (2.0, 0.0); };
    zoomIn.onClick = [this] { state.zoom (0.5, 0.0); };
    addAndMakeVisible (zoomOut);
    addAndMakeVisible (zoomIn);

    gridLabel.setFont (theme::font (12.0f));
    gridLabel.setColour (juce::Label::textColourId, theme::textDim);
    addAndMakeVisible (gridLabel);

    state.addChangeListener (this);
    refresh();
}

EditToolbar::~EditToolbar()
{
    state.removeChangeListener (this);
}

void EditToolbar::changeListenerCallback (juce::ChangeBroadcaster*)
{
    refresh();
}

void EditToolbar::refresh()
{
    for (int i = 0; i < modeButtons.size(); ++i)
        modeButtons[i]->setToggleState (modes[i].mode == state.mode, juce::dontSendNotification);
    for (int i = 0; i < toolButtons.size(); ++i)
        toolButtons[i]->setToggleState (tools[i].tool == state.tool, juce::dontSendNotification);

    gridLabel.setText ("Grid: 1 beat @ " + juce::String (state.tempo, std::abs (state.tempo - std::round (state.tempo)) < 0.005 ? 0 : 2) + " BPM",
                       juce::dontSendNotification);
}

void EditToolbar::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);
    g.setColour (theme::divider);
    g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());
}

void EditToolbar::resized()
{
    auto area = getLocalBounds().reduced (8, 5);

    for (auto* b : modeButtons)
        b->setBounds (area.removeFromLeft (64).reduced (1, 0));

    area.removeFromLeft (18);
    for (auto* b : toolButtons)
        b->setBounds (area.removeFromLeft (56).reduced (1, 0));

    area.removeFromLeft (18);
    zoomOut.setBounds (area.removeFromLeft (28).reduced (1, 0));
    zoomIn.setBounds (area.removeFromLeft (28).reduced (1, 0));

    area.removeFromLeft (12);
    gridLabel.setBounds (area.removeFromLeft (200));
}

} // namespace meridian
